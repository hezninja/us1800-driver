/*
 * us1800d.c - userspace daemon for the TASCAM US-1800 (see PROTOCOL.md).
 *
 * Owns the USB device, keeps the isochronous playback stream running (the device only sends capture data while it
 * is), decodes the bulk-IN capture stream, and exchanges audio with clients through the shared rings in
 * us1800_shm.h.
 *
 * Build: clang -O2 -o build/us1800d src/us1800d.c -framework IOKit -framework CoreFoundation -lpthread
 * Run:   ./build/us1800d [--out-channels N] [--rate 48000] [--queue-ms 6] [--prefill 120] [--cushion 160]
 */
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include "us1800_shm.h"

#define VID 0x0644
#define PID 0x8030
#define FRAMES_PER_PKT 6        /* sample frames per 125 us microframe at 48 kHz */
#define ISOC_FRAMES 8           /* microframes per transfer */
#define MAX_XFERS 64            /* 1 ms each; --queue-ms picks how many are kept in flight (wire-side latency) */
#define FRAMES_PER_XFER (FRAMES_PER_PKT * ISOC_FRAMES)

typedef struct { uint8_t *audio; IOUSBLowLatencyIsocFrame *frames; UInt64 start; } Xfer;

static us1800_shm *g_shm;
static IOUSBDeviceInterface300 **g_dev;
static IOUSBInterfaceInterface300 **g_if0, **g_if1;
static int g_pipe_out, g_pipe_cap, g_out_ch = 4, g_rate = 48000;
static Xfer g_x[MAX_XFERS];
static int g_nx = 9;                        /* ms of audio queued on the wire; the realtime timer thread's worst measured hiccup is ~3-4 ms */
static uint32_t g_prefill = 200, g_cushion = 240;   /* plug-in ring targets in frames (see us1800_plugin.c) */
static UInt64 g_next;
static volatile sig_atomic_t g_run = 1;

static void on_signal(int s) { (void)s; g_run = 0; }

/* ---- USB open helpers -------------------------------------------------------------------------------------- */

static CFMutableDictionaryRef us1800_match(void)
{
    CFMutableDictionaryRef m = IOServiceMatching(kIOUSBDeviceClassName);
    SInt32 v = VID, p = PID;
    CFNumberRef nv = CFNumberCreate(NULL, kCFNumberSInt32Type, &v), np = CFNumberCreate(NULL, kCFNumberSInt32Type, &p);
    CFDictionarySetValue(m, CFSTR(kUSBVendorID), nv);
    CFDictionarySetValue(m, CFSTR(kUSBProductID), np);
    CFRelease(nv); CFRelease(np);
    return m;
}

static IOUSBDeviceInterface300 **open_device(void)
{
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, us1800_match());
    if (!svc) return NULL;
    IOCFPlugInInterface **plug = NULL; SInt32 score;
    IOCreatePlugInInterfaceForService(svc, kIOUSBDeviceUserClientTypeID, kIOCFPlugInInterfaceID, &plug, &score);
    IOObjectRelease(svc);
    if (!plug) return NULL;
    IOUSBDeviceInterface300 **dev = NULL;
    (*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID300), (LPVOID *)&dev);
    (*plug)->Release(plug);
    return dev;
}

static IOUSBInterfaceInterface300 **open_interface(IOUSBDeviceInterface300 **dev, int want)
{
    IOUSBFindInterfaceRequest req = { kIOUSBFindInterfaceDontCare, kIOUSBFindInterfaceDontCare,
                                      kIOUSBFindInterfaceDontCare, kIOUSBFindInterfaceDontCare };
    io_iterator_t it; (*dev)->CreateInterfaceIterator(dev, &req, &it);
    io_service_t s; int n = 0; IOUSBInterfaceInterface300 **iface = NULL;
    while ((s = IOIteratorNext(it))) {
        if (n == want) {
            IOCFPlugInInterface **plug = NULL; SInt32 score;
            IOCreatePlugInInterfaceForService(s, kIOUSBInterfaceUserClientTypeID, kIOCFPlugInInterfaceID, &plug, &score);
            if (plug) {
                (*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID300), (LPVOID *)&iface);
                (*plug)->Release(plug);
            }
        }
        IOObjectRelease(s); n++;
    }
    IOObjectRelease(it);
    return iface;
}

static IOReturn vendor(uint8_t rt, uint8_t req, uint16_t val, uint16_t idx, void *d, uint16_t len)
{
    IOUSBDevRequest r = { rt, req, val, idx, len, d, 0 };
    return (*g_dev)->DeviceRequest(g_dev, &r);
}

static int pipe_for(IOUSBInterfaceInterface300 **ifc, uint8_t addr)
{
    UInt8 n; (*ifc)->GetNumEndpoints(ifc, &n);
    for (UInt8 i = 1; i <= n; i++) {
        UInt8 dir, num, tt, iv; UInt16 mp;
        (*ifc)->GetPipeProperties(ifc, i, &dir, &num, &tt, &mp, &iv);
        if ((((dir == kUSBIn) ? 0x80 : 0) | num) == addr) return i;
    }
    return -1;
}

/* ---- playback: isoc OUT ------------------------------------------------------------------------------------ */

static FILE *g_dump;     /* debug: US1800_DUMP=path writes the exact S24_3LE bytes sent on the wire */

/* Time-constraint (realtime) scheduling, as audio drivers use: ~0.2 ms of work every 1 ms. */
static void make_realtime(void)
{
    mach_timebase_info_data_t tb; mach_timebase_info(&tb);
    double ns2abs = (double)tb.denom / tb.numer;
    thread_time_constraint_policy_data_t p = {
        .period = (uint32_t)(1e6 * ns2abs), .computation = (uint32_t)(2e5 * ns2abs),
        .constraint = (uint32_t)(5e5 * ns2abs), .preemptible = 1 };
    kern_return_t kr = thread_policy_set(mach_thread_self(), THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&p,
                                         THREAD_TIME_CONSTRAINT_POLICY_COUNT);
    if (kr != KERN_SUCCESS) fprintf(stderr, "[us1800d] realtime policy failed: %d\n", kr);
}

static unsigned g_late;     /* transfers that missed their frame and had to be rescheduled (each is an audible hole) */

/* Restart the isoc schedule `ahead` USB frames (1 ms each) from now. */
static void resync(int ahead)
{
    UInt64 f; AbsoluteTime t;
    (*g_if0)->GetBusFrameNumber(g_if0, &f, &t);
    g_next = f + ahead;
}

/* ---- rate matching ---------------------------------------------------------------------------------------------- *
 * The unit's sample clock is a few tens of ppm slower than the USB frame clock, so a fixed 48 frames/ms slowly overfills its
 * playback FIFO; every ~50 s it overflows and plays garbage for several seconds. Isoc IN 0x81 reports how many frames the unit
 * consumed in each ms (47/48/49, see PROTOCOL.md). We keep (frames sent - frames consumed) at the level it had when we locked,
 * by sending that average rate plus a gentle correction. */

static volatile double g_fb_rate = 48.0;    /* frames the unit consumes per ms, smoothed over ~2 s */
static volatile uint64_t g_fb_sum;          /* frames the unit has reported consuming */
static volatile unsigned g_fb_valid;        /* valid feedback packets seen */
static uint64_t g_sent_total;               /* frames we have queued on the wire */
static double g_fifo_ref, g_fifo_dev, g_acc;
static int g_fifo_locked;

static int plan_xfer(int cnt[ISOC_FRAMES])
{
    int n = FRAMES_PER_XFER;
    if (g_fb_valid >= 200) {
        double level = (double)g_sent_total - (double)g_fb_sum;
        if (!g_fifo_locked) { g_fifo_ref = level; g_fifo_locked = 1; }
        g_fifo_dev = level - g_fifo_ref;                    /* > 0: we have sent more than the unit consumed */
        g_acc += g_fb_rate - 0.0002 * g_fifo_dev;
        n = (int)floor(g_acc); g_acc -= n;
        if (n < FRAMES_PER_XFER - 2) n = FRAMES_PER_XFER - 2;
        if (n > FRAMES_PER_XFER + 2) n = FRAMES_PER_XFER + 2;
    }
    g_sent_total += n;
    int base = n / ISOC_FRAMES, rem = n % ISOC_FRAMES;
    for (int i = 0; i < ISOC_FRAMES; i++) cnt[i] = base + (i >= ISOC_FRAMES - rem ? 1 : 0);
    return n;
}

/* Pull `total` frames from the playback ring (silence on underrun), pack as S24_3LE. */
static void fill_from_ring(Xfer *x, int total)
{
    uint64_t r = atomic_load_explicit(&g_shm->play_r, memory_order_relaxed);
    uint64_t w = atomic_load_explicit(&g_shm->play_w, memory_order_acquire);
    uint32_t under = 0;
    for (int f = 0; f < total; f++) {
        const int32_t *src = NULL;
        if (r < w) src = g_shm->play[r & (US1800_RING_FRAMES - 1)];
        else under++;
        for (int c = 0; c < g_out_ch; c++) {
            int32_t v = src ? (src[c] >> 8) : 0;
            uint8_t *p = x->audio + (f * g_out_ch + c) * 3;
            p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; p[2] = (v >> 16) & 0xff;
        }
        if (src) r++;
    }
    atomic_store_explicit(&g_shm->play_r, r, memory_order_release);
    if (under) atomic_fetch_add(&g_shm->underruns, under);
}

static void submit(Xfer *x);
static volatile int g_resync_req;
static double g_cb_delay_max_ms, g_ts_gap_max_ms;       /* delivery delay of completions, and gaps between the kernel's own completion stamps */
static uint64_t g_ts_last;
static void on_done(void *ref, IOReturn r, void *a0)
{
    (void)a0;
    {   /* kernel completion stamp of the last microframe vs now */
        static mach_timebase_info_data_t tb; if (!tb.denom) mach_timebase_info(&tb);
        IOUSBLowLatencyIsocFrame *fl = ((Xfer *)ref)->frames;
        uint64_t ts = ((uint64_t)fl[ISOC_FRAMES - 1].frTimeStamp.hi << 32) | fl[ISOC_FRAMES - 1].frTimeStamp.lo, now = mach_absolute_time();
        if (ts && now > ts) { double d = (double)(now - ts) * tb.numer / tb.denom / 1e6; if (d > g_cb_delay_max_ms) g_cb_delay_max_ms = d; }
        if (ts && g_ts_last && ts > g_ts_last) { double g = (double)(ts - g_ts_last) * tb.numer / tb.denom / 1e6; if (g > g_ts_gap_max_ms) g_ts_gap_max_ms = g; }
        if (ts) g_ts_last = ts;
    }
    /* Completions reach us 0.3-9 ms late, so they must not drive the stream (out_thread does); they only report errors. */
    if (r != kIOReturnSuccess) {
        atomic_fetch_add(&g_shm->usb_errors, 1); fprintf(stderr, "[us1800d] isoc OUT completion error 0x%x\n", r);
        g_resync_req = 1;
    }
}

static double secs_now(void)
{
    static mach_timebase_info_data_t tb; static uint64_t t0;
    if (!tb.denom) { mach_timebase_info(&tb); t0 = mach_absolute_time(); }
    return (double)(mach_absolute_time() - t0) * tb.numer / tb.denom / 1e9;
}

static void submit(Xfer *x)
{
    int cnt[ISOC_FRAMES], total = plan_xfer(cnt);
    fill_from_ring(x, total);
    if (g_dump) fwrite(x->audio, 1, total * g_out_ch * 3, g_dump);
    for (int i = 0; i < ISOC_FRAMES; i++) {
        x->frames[i].frStatus = 0;
        x->frames[i].frReqCount = cnt[i] * g_out_ch * 3;
        x->frames[i].frActCount = 0;
        x->frames[i].frTimeStamp.hi = 0; x->frames[i].frTimeStamp.lo = 0;
    }
    x->start = g_next++;
    kern_return_t kr = (*g_if0)->LowLatencyWriteIsochPipeAsync(g_if0, g_pipe_out, x->audio, x->start,
                                                               ISOC_FRAMES, 1, x->frames, on_done, x);
    if (kr == (kern_return_t)0xe00002ee) {          /* start frame already in the past: realign and retry */
        /* The queue ran dry, so nothing is in flight: restart just ahead of now (a 4 ms hole, not 50). */
        g_late++; fprintf(stderr, "[us1800d] %.3f s: playback transfer late, rescheduling\n", secs_now()); resync(g_nx + 2); x->start = g_next++;
        kr = (*g_if0)->LowLatencyWriteIsochPipeAsync(g_if0, g_pipe_out, x->audio, x->start,
                                                     ISOC_FRAMES, 1, x->frames, on_done, x);
    }
    if (kr != kIOReturnSuccess) atomic_fetch_add(&g_shm->usb_errors, 1);
}

/* ---- capture: bulk IN 0x86 --------------------------------------------------------------------------------- */

/* One 128-byte frame = 64 little-endian words of bit-planes (PROTOCOL.md). Output is 24-bit << 8. */
static void decode_frame(const uint8_t *p, int32_t out[US1800_IN_CH])
{
    uint32_t a[9] = {0}, b[9] = {0};
    for (int k = 0; k < 24; k++) {
        uint16_t wa = p[2 * k] | (p[2 * k + 1] << 8);
        uint16_t wb = p[2 * (32 + k)] | (p[2 * (32 + k) + 1] << 8);
        for (int j = 0; j < 9; j++) {
            a[j] = (a[j] << 1) | ((wa >> j) & 1);
            b[j] = (b[j] << 1) | ((wb >> j) & 1);
        }
    }
    for (int j = 0; j < 9; j++) { out[2 * j] = (int32_t)(a[j] << 8); out[2 * j + 1] = (int32_t)(b[j] << 8); }
}

/* ---- device init (sequence in PROTOCOL.md) ------------------------------------------------------------------ */

static int init_device(void)
{
    g_dev = open_device();
    if (!g_dev) { fprintf(stderr, "US-1800 not found\n"); return 1; }
    (*g_dev)->USBDeviceOpen(g_dev); (*g_dev)->SetConfiguration(g_dev, 1);
    g_if0 = open_interface(g_dev, 0); g_if1 = open_interface(g_dev, 1);
    if (!g_if0 || !g_if1) { fprintf(stderr, "cannot open interfaces\n"); return 1; }
    (*g_if0)->USBInterfaceOpen(g_if0); (*g_if1)->USBInterfaceOpen(g_if1);

    uint8_t fw[2] = {0}; vendor(0xC0, 0x56, 0, 0, fw, 2);
    fprintf(stderr, "US-1800 firmware %d.%d\n", fw[1], fw[0]);
    (*g_if0)->SetAlternateInterface(g_if0, 1); (*g_if1)->SetAlternateInterface(g_if1, 1);
    uint8_t freq[3] = { g_rate & 0xff, (g_rate >> 8) & 0xff, (g_rate >> 16) & 0xff };
    vendor(0x22, 0x01, 0x0100, 0x83, freq, 3); vendor(0x22, 0x01, 0x0100, 0x02, freq, 3);
    usleep(50000);
    uint8_t reg = 0; vendor(0xC0, 0x49, 0, 0, &reg, 1);
    vendor(0x40, 0x49, (reg & ~0x20) | 0x10, 0, NULL, 0);        /* >= 17 capture channels: 32-word frame layout */

    g_pipe_out = pipe_for(g_if0, 0x02); g_pipe_cap = pipe_for(g_if1, 0x86);
    if (g_pipe_out < 0 || g_pipe_cap < 0) { fprintf(stderr, "endpoints not found\n"); return 1; }
    return 0;
}

/* ---- unplug handling ---------------------------------------------------------------------------------------- *
 * On unplug the daemon exits and launchd restarts it; the new instance waits for the unit to come back and starts clean.
 * The shm object is never unlinked, so the plug-in in coreaudiod stays attached across restarts (see shm_create). */

static void on_terminated(void *ref, io_iterator_t it)
{
    (void)ref;
    io_service_t s; int gone = 0;
    while ((s = IOIteratorNext(it))) { IOObjectRelease(s); gone = 1; }
    if (!gone) return;
    atomic_store(&g_shm->running, 0);
    fprintf(stderr, "[us1800d] US-1800 unplugged, exiting\n");
    _exit(0);
}

static void watch_unplug(void)
{
    IONotificationPortRef np = IONotificationPortCreate(kIOMainPortDefault);
    CFRunLoopAddSource(CFRunLoopGetCurrent(), IONotificationPortGetRunLoopSource(np), kCFRunLoopDefaultMode);
    io_iterator_t it;
    IOServiceAddMatchingNotification(np, kIOTerminatedNotification, us1800_match(), on_terminated, NULL, &it);
    io_service_t d; while ((d = IOIteratorNext(it))) IOObjectRelease(d);   /* arm the notification */
}

/* ---- shared memory ------------------------------------------------------------------------------------------ */

static us1800_shm *shm_create(void)
{
    int fd = shm_open(US1800_SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (fd < 0) { perror("shm_open"); return NULL; }
    fchmod(fd, 0666);
    struct stat st;     /* macOS rejects ftruncate on a shm object that is already sized, so only size a fresh one */
    if (fstat(fd, &st) != 0 || (st.st_size < (off_t)sizeof(us1800_shm) && ftruncate(fd, sizeof(us1800_shm)) != 0)) { perror("ftruncate"); close(fd); return NULL; }
    us1800_shm *s = mmap(NULL, sizeof *s, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (s == MAP_FAILED) { perror("mmap"); return NULL; }
    memset(s, 0, sizeof *s);        /* an existing object is reused: the plug-in's mapping stays valid and resyncs to the reset counters */
    s->magic = US1800_SHM_MAGIC; s->version = US1800_SHM_VERSION; s->rate = g_rate; s->out_ch = g_out_ch;
    return s;
}

/* Capture must keep several bulk reads in flight: with one synchronous read at a time the pipe idles between requests,
 * the unit's small FIFO overflows and ~1% of the audio is silently lost (measured: 47.5 kHz instead of 48 kHz). */
static int g_cap_err_streak;
static double g_cap_gap_max_ms;     /* longest silence between capture completions in the current 10 s window */
#define CAP_ASYNC_N 64              /* 64 x 1 KB = ~114 ms of audio in flight: a late completion must not make the unit's FIFO overflow */
#define CAP_ASYNC_BUF 1024
static uint8_t g_cbuf[CAP_ASYNC_N][CAP_ASYNC_BUF];

static void cap_async_done(void *ref, IOReturn r, void *a0)
{
    int i = (int)(intptr_t)ref; UInt32 sz = (UInt32)(uintptr_t)a0;
    {   static uint64_t last; static mach_timebase_info_data_t tb; if (!tb.denom) mach_timebase_info(&tb);
        uint64_t now = mach_absolute_time();
        if (last) { double g = (double)(now - last) * tb.numer / tb.denom / 1e6; if (g > g_cap_gap_max_ms) g_cap_gap_max_ms = g; }
        last = now; }
    if (r == kIOReturnSuccess && sz >= 128) {
        g_cap_err_streak = 0;
        uint64_t w = atomic_load_explicit(&g_shm->cap_w, memory_order_relaxed);
        for (UInt32 off = 0; off + 128 <= sz; off += 128, w++)
            decode_frame(g_cbuf[i] + off, g_shm->cap[w & (US1800_RING_FRAMES - 1)]);
        atomic_store_explicit(&g_shm->cap_w, w, memory_order_release);
    } else {
        atomic_fetch_add(&g_shm->usb_errors, 1);
        fprintf(stderr, "[us1800d] %.3f s: capture error 0x%x on read %d\n", secs_now(), r, i);
        /* Aborted reads (0xe00002eb) are fallout from a clear: just requeue them. A timeout halts the pipe: clear it. */
        if (r != (IOReturn)0xe00002eb) (*g_if1)->ClearPipeStallBothEnds(g_if1, g_pipe_cap);   /* a timeout halts the pipe */
    }
    if (g_run) (*g_if1)->ReadPipeAsync(g_if1, g_pipe_cap, g_cbuf[i], CAP_ASYNC_BUF, cap_async_done, ref);
}

/* ---- isoc IN 0x81: device timing packets (research; enabled with --fb-log) ------------------------------------ */

#define FB_XFERS  4
#define FB_FRAMES 8
typedef struct { IOUSBIsocFrame fr[FB_FRAMES]; uint8_t buf[FB_FRAMES * 64]; } FbX;
static FbX g_fb[FB_XFERS];
static int g_fb_on, g_pipe_fb, g_fb_logged;
static UInt64 g_fb_next;
static unsigned g_fb_len[65], g_fb_err, g_fb_byte[256], g_fb_odd, g_fb_pkts;
static uint32_t g_fb_val[16]; static unsigned g_fb_cnt[16];

static void fb_done(void *ref, IOReturn r, void *a0);
static void fb_start(FbX *x)
{
    UInt64 f; AbsoluteTime t; (*g_if1)->GetBusFrameNumber(g_if1, &f, &t);
    if (g_fb_next < f + 4) g_fb_next = f + 10;
    for (int i = 0; i < FB_FRAMES; i++) { x->fr[i].frStatus = 0; x->fr[i].frReqCount = 64; x->fr[i].frActCount = 0; }
    memset(x->buf, 0, sizeof x->buf);
    kern_return_t kr = (*g_if1)->ReadIsochPipeAsync(g_if1, g_pipe_fb, x->buf, g_fb_next, FB_FRAMES, x->fr, fb_done, x);
    if (kr != KERN_SUCCESS) { g_fb_err++; if (g_fb_err < 5) fprintf(stderr, "[fb] submit failed 0x%x\n", kr); }
    g_fb_next += FB_FRAMES;
}

static unsigned g_fb_missing;      /* consecutive empty 1 ms slots; the next packet's older bytes carry those counts */

static void fb_account(unsigned c)
{
    if (c < 40 || c > 56) return;
    g_fb_sum += c; g_fb_valid++;
    g_fb_rate += ((double)c - g_fb_rate) * (1.0 / 2000.0);
}

static void fb_done(void *ref, IOReturn r, void *a0)
{
    (void)a0;
    FbX *x = ref;
    if (r != kIOReturnSuccess) { g_fb_err++; if (g_fb_err < 5) fprintf(stderr, "[fb] completion error 0x%x\n", r); }
    for (int i = 0; i < FB_FRAMES; i++) {
        unsigned n = x->fr[i].frActCount; if (n > 64) n = 64;
        g_fb_len[n]++;
        const uint8_t *pk = x->buf + i * 64;
        if (n == 0) { if (g_fb_valid) g_fb_missing++; }
        else if (n >= 3) {
            /* a packet is [newest, previous, one before]: recover the counts of up to two empty slots before it */
            if (g_fb_missing == 1) fb_account(pk[1]);
            else if (g_fb_missing == 2) { fb_account(pk[2]); fb_account(pk[1]); }
            g_fb_missing = 0;
            fb_account(pk[0]);
        }
        if (g_fb_on && n) {
            g_fb_pkts++;
            int odd = 0;
            for (unsigned j = 0; j < n; j++) { g_fb_byte[x->buf[i * 64 + j]]++; if (x->buf[i * 64 + j] != 0x30) odd = 1; }
            if (odd && g_fb_pkts > 20 && g_fb_odd++ < 40) {
                fprintf(stderr, "[fb] pkt %u (%.2f s): act=%u:", g_fb_pkts, g_fb_pkts / 1000.0, n);
                for (unsigned j = 0; j < n; j++) fprintf(stderr, " %02x", x->buf[i * 64 + j]);
                fprintf(stderr, "\n");
            }
        }
        if (g_fb_on && n >= 4) {
            uint32_t v; memcpy(&v, x->buf + i * 64, 4);
            int k = 0; while (k < 16 && g_fb_cnt[k] && g_fb_val[k] != v) k++;
            if (k < 16) { g_fb_val[k] = v; g_fb_cnt[k]++; }
        }
        if (g_fb_on && g_fb_logged < 8 && n) {
            g_fb_logged++;
            fprintf(stderr, "[fb] st=0x%x act=%u:", x->fr[i].frStatus, n);
            for (unsigned j = 0; j < n && j < 24; j++) fprintf(stderr, " %02x", x->buf[i * 64 + j]);
            fprintf(stderr, "\n");
        }
    }
    if (g_run) fb_start(x);
}

static void fb_report(void)
{
    fprintf(stderr, "[fb] packet length histogram:");
    for (int n = 0; n <= 64; n++) if (g_fb_len[n]) fprintf(stderr, " %d:%u", n, g_fb_len[n]);
    fprintf(stderr, "\n[fb] byte values:");
    for (int b = 0; b < 256; b++) if (g_fb_byte[b]) fprintf(stderr, " 0x%02x:%u", b, g_fb_byte[b]);
    fprintf(stderr, "\n[fb] first-uint32 values:");
    for (int k = 0; k < 16 && g_fb_cnt[k]; k++) fprintf(stderr, " 0x%x:%u", g_fb_val[k], g_fb_cnt[k]);
    fprintf(stderr, "\n[fb] errors: %u\n", g_fb_err);
}

static void *capture_thread(void *arg)
{
    (void)arg;
    make_realtime();
    CFRunLoopSourceRef src; (*g_if1)->CreateInterfaceAsyncEventSource(g_if1, &src);
    CFRunLoopAddSource(CFRunLoopGetCurrent(), src, kCFRunLoopDefaultMode);
    (*g_if1)->ClearPipeStallBothEnds(g_if1, g_pipe_cap);
    for (int i = 0; i < CAP_ASYNC_N; i++)
        (*g_if1)->ReadPipeAsync(g_if1, g_pipe_cap, g_cbuf[i], CAP_ASYNC_BUF, cap_async_done, (void *)(intptr_t)i);
    g_pipe_fb = pipe_for(g_if1, 0x81);
    for (int i = 0; i < FB_XFERS; i++) fb_start(&g_fb[i]);
    while (g_run) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, false);
    return NULL;
}


/* The output stream is driven by a realtime timer, not by completion callbacks: timers wake within ~0.7 ms, while completions
 * arrive up to 9 ms late (the kernel's own stamps show the bus itself never stalls). Every ms, top the wire queue back up. */
#define OUT_SLOTS 32
static double g_out_wake_max_ms, g_out_call_max_ms;        /* worst timer wake-up lateness, and worst kernel call, per 10 s */
static void *out_thread(void *arg)
{
    (void)arg;
    make_realtime();
    mach_timebase_info_data_t tb; mach_timebase_info(&tb);
    uint64_t step = (uint64_t)(1e6 * (double)tb.denom / (double)tb.numer), next = mach_absolute_time() + step;
    int slot = 0;
    resync(2);
    while (g_run) {
        mach_wait_until(next); next += step;
        uint64_t now = mach_absolute_time();
        { double late = (double)(now - (next - step)) * tb.numer / tb.denom / 1e6; if (late > g_out_wake_max_ms) g_out_wake_max_ms = late; }
        if (now > next + 5 * step) next = now + step;                  /* fell far behind: do not burst */
        UInt64 f; AbsoluteTime t; (*g_if0)->GetBusFrameNumber(g_if0, &f, &t);
        { double d = (double)(mach_absolute_time() - now) * tb.numer / tb.denom / 1e6; if (d > g_out_call_max_ms) g_out_call_max_ms = d; }
        if (g_resync_req) { g_resync_req = 0; g_next = f + g_nx + 2; }
        while (g_run && (int64_t)(g_next - f) < (int64_t)g_nx + 1) {
            uint64_t s0 = mach_absolute_time(); submit(&g_x[slot]); slot = (slot + 1) % OUT_SLOTS;
            double d = (double)(mach_absolute_time() - s0) * tb.numer / tb.denom / 1e6; if (d > g_out_call_max_ms) g_out_call_max_ms = d;
        }
    }
    return NULL;
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--out-channels") && i + 1 < argc) g_out_ch = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--rate") && i + 1 < argc) g_rate = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--fb-log")) g_fb_on = 1;
        else if (!strcmp(argv[i], "--queue-ms") && i + 1 < argc) g_nx = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--prefill") && i + 1 < argc) g_prefill = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--cushion") && i + 1 < argc) g_cushion = atoi(argv[++i]);
    }
    if (g_out_ch < 2 || g_out_ch > US1800_MAX_OUT) { fprintf(stderr, "--out-channels must be 2..%d\n", US1800_MAX_OUT); return 1; }
    if (g_nx < 2 || g_nx > MAX_XFERS || g_prefill > 4096 || g_cushion > 4096) { fprintf(stderr, "--queue-ms 2..%d, --prefill/--cushion 0..4096\n", MAX_XFERS); return 1; }
    signal(SIGINT, on_signal); signal(SIGTERM, on_signal);

    g_shm = shm_create();
    if (!g_shm) return 1;
    {   io_service_t d; int waited = 0;     /* idle until the unit is plugged in; no log spam, launchd keeps us alive */
        while (g_run && !(d = IOServiceGetMatchingService(kIOMainPortDefault, us1800_match()))) { if (!waited++) fprintf(stderr, "waiting for US-1800\n"); sleep(1); }
        if (!g_run) return 0;
        IOObjectRelease(d);
        if (waited) sleep(2);               /* let a freshly enumerated unit settle */
    }
    if (init_device() != 0) return 1;
    watch_unplug();
    g_shm->play_prefill = g_prefill; g_shm->cap_cushion = g_cushion;

    for (int i = 0; i < OUT_SLOTS; i++) {
        void *a, *f;
        (*g_if0)->LowLatencyCreateBuffer(g_if0, &a, (FRAMES_PER_XFER + 4) * US1800_MAX_OUT * 3, kUSBLowLatencyWriteBuffer);
        (*g_if0)->LowLatencyCreateBuffer(g_if0, &f, ISOC_FRAMES * sizeof(IOUSBLowLatencyIsocFrame), kUSBLowLatencyFrameListBuffer);
        g_x[i].audio = a; g_x[i].frames = f;
    }
    CFRunLoopSourceRef src; (*g_if0)->CreateInterfaceAsyncEventSource(g_if0, &src);
    CFRunLoopAddSource(CFRunLoopGetCurrent(), src, kCFRunLoopDefaultMode);

    if (getenv("US1800_DUMP")) g_dump = fopen(getenv("US1800_DUMP"), "wb");
    pthread_t cap, outt; pthread_create(&cap, NULL, capture_thread, NULL); pthread_create(&outt, NULL, out_thread, NULL);
    atomic_store(&g_shm->running, 1);
    fprintf(stderr, "us1800d running: %d Hz, %d playback channels, 18 capture slots, queue %d ms, prefill %u, cushion %u\n", g_rate, g_out_ch, g_nx, g_prefill, g_cushion);

    time_t last = time(NULL);
    while (g_run) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, false);
        if (time(NULL) - last >= 10) {
            last = time(NULL);
            fprintf(stderr, "[us1800d] cap_frames=%llu play_frames=%llu underruns=%u usb_errors=%u plug_resyncs=%u late_xfers=%u fb_rate=%.4f fifo_dev=%+.0f fb_valid=%u cb_delay_max=%.2fms kernel_stamp_gap_max=%.2fms cap_cb_gap_max=%.2fms out_wake_max=%.2fms out_call_max=%.2fms\n",
                    (unsigned long long)atomic_load(&g_shm->cap_w), (unsigned long long)atomic_load(&g_shm->play_r),
                    atomic_load(&g_shm->underruns), atomic_load(&g_shm->usb_errors), atomic_load(&g_shm->plug_resyncs), g_late, g_fb_rate, g_fifo_dev, g_fb_valid, g_cb_delay_max_ms, g_ts_gap_max_ms, g_cap_gap_max_ms, g_out_wake_max_ms, g_out_call_max_ms);
            g_cb_delay_max_ms = g_ts_gap_max_ms = g_cap_gap_max_ms = g_out_wake_max_ms = g_out_call_max_ms = 0;
        }
    }
    if (g_fb_on) fb_report();
    atomic_store(&g_shm->running, 0);
    pthread_join(cap, NULL); pthread_join(outt, NULL);
    for (int i = 0; i < 5; i++) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, false);
    (*g_if0)->USBInterfaceClose(g_if0); (*g_if1)->USBInterfaceClose(g_if1); (*g_dev)->USBDeviceClose(g_dev);
    fprintf(stderr, "us1800d stopped\n");
    return 0;
}
