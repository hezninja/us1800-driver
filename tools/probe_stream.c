/*
 * probe_stream.c - run the isoc OUT stream (silence or tone) and, concurrently, read bulk IN 0x83 on a
 * thread. Writes everything read to build/cap.bin and prints the first packets. Exploratory.
 *
 * Build: clang -O1 -o build/probe_stream tools/probe_stream.c -framework IOKit -framework CoreFoundation -lpthread
 * Run:   ./build/probe_stream [out_channels=4] [seconds=4] [tone_amplitude=0..1] [no_stream=0|1] [sweep=0|1|10+k: only output k+1]
 */
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <math.h>
#include <stdint.h>
#include <time.h>

#define VID 0x0644
#define PID 0x8030
#define FRAMES_PER_PKT 6
#define ISOC_FRAMES 8
#define NUM_XFERS 8
#define RATE 48000

typedef struct { uint8_t *audio; IOUSBLowLatencyIsocFrame *frames; UInt64 start; } Xfer;

static IOUSBInterfaceInterface300 **g_if0, **g_if1;
static int g_pipe_out, g_pipe_bulk_in, g_ch = 4, g_running = 1;
static double g_tone = 0.0;  /* tone amplitude 0..1 (0 = silence) */
static Xfer g_x[NUM_XFERS];
static UInt64 g_next;
static double g_phase;
static long g_frames_out;  /* frames queued, drives the per-output sweep */
static int g_sweep = 0;    /* 1 = tone on one output at a time, 3 s each */
static long g_done, g_errs;

static IOUSBDeviceInterface300 **open_device(void)
{
    CFMutableDictionaryRef m = IOServiceMatching(kIOUSBDeviceClassName);
    SInt32 v = VID, p = PID;
    CFNumberRef nv = CFNumberCreate(NULL, kCFNumberSInt32Type, &v), np = CFNumberCreate(NULL, kCFNumberSInt32Type, &p);
    CFDictionarySetValue(m, CFSTR(kUSBVendorID), nv);
    CFDictionarySetValue(m, CFSTR(kUSBProductID), np);
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, m);
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

static IOReturn vendor(IOUSBDeviceInterface300 **dev, uint8_t rt, uint8_t req, uint16_t val, uint16_t idx, void *d, uint16_t len)
{
    IOUSBDevRequest r = { rt, req, val, idx, len, d, 0 };
    return (*dev)->DeviceRequest(dev, &r);
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

static void resync(void) { UInt64 f; AbsoluteTime t; (*g_if0)->GetBusFrameNumber(g_if0, &f, &t); g_next = f + 50; }

static void fill(Xfer *x)
{
    int n = ISOC_FRAMES * FRAMES_PER_PKT;
    for (int f = 0; f < n; f++) {
        int32_t s = 0;
        if (g_tone > 0.0) { s = (int32_t)(sin(2 * M_PI * 440.0 * g_phase / RATE) * g_tone * 8388607.0); g_phase += 1; }
        int seg = g_sweep >= 10 ? g_sweep - 10 : (g_sweep ? (int)((g_frames_out / (3L * RATE)) % g_ch) : -1);
        g_frames_out++;
        for (int c = 0; c < g_ch; c++) {
            int32_t v = (g_sweep && c != seg) ? 0 : s;
            uint8_t *p = x->audio + (f * g_ch + c) * 3;
            p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; p[2] = (v >> 16) & 0xff;
        }
    }
}

static void submit(Xfer *x);
static void on_done(void *ref, IOReturn r, void *a0)
{
    (void)a0; g_done++;
    if (r != kIOReturnSuccess) { g_errs++; resync(); }
    if (g_running) submit((Xfer *)ref);
}

static void submit(Xfer *x)
{
    fill(x);
    for (int i = 0; i < ISOC_FRAMES; i++) {
        x->frames[i].frStatus = 0; x->frames[i].frReqCount = FRAMES_PER_PKT * g_ch * 3; x->frames[i].frActCount = 0;
        x->frames[i].frTimeStamp.hi = 0; x->frames[i].frTimeStamp.lo = 0;
    }
    x->start = g_next++;
    kern_return_t kr = (*g_if0)->LowLatencyWriteIsochPipeAsync(g_if0, g_pipe_out, x->audio, x->start,
                                                               ISOC_FRAMES, 1, x->frames, on_done, x);
    if (kr == (kern_return_t)0xe00002ee) {
        resync(); x->start = g_next++;
        kr = (*g_if0)->LowLatencyWriteIsochPipeAsync(g_if0, g_pipe_out, x->audio, x->start,
                                                     ISOC_FRAMES, 1, x->frames, on_done, x);
    }
    if (kr != kIOReturnSuccess) g_errs++;
}

typedef struct { IOUSBInterfaceInterface300 **ifc; int pipe; const char *name; const char *file; } RArg;

static void *reader(void *arg)
{
    RArg *a = (RArg *)arg;
    FILE *f = fopen(a->file, "wb");
    long total = 0; int reads = 0, errs = 0;
    (*a->ifc)->ClearPipeStallBothEnds(a->ifc, a->pipe);
    while (g_running) {
        uint8_t buf[512]; UInt32 sz = sizeof buf;
        IOReturn r = (*a->ifc)->ReadPipeTO(a->ifc, a->pipe, buf, &sz, 300, 600);
        if (r == kIOReturnSuccess && sz) {
            if (reads < 4) {
                printf("[%s] read #%d: %u bytes\n", a->name, reads, (unsigned)sz);
                for (UInt32 i = 0; i < sz && i < 128; i++) printf("%02x%s", buf[i], (i % 32 == 31) ? "\n" : " ");
                printf("\n");
            }
            fwrite(buf, 1, sz, f); total += sz; reads++;
        } else {
            if (++errs <= 3) printf("[%s] read err 0x%x\n", a->name, r);
            (*a->ifc)->ClearPipeStallBothEnds(a->ifc, a->pipe);
            usleep(20000);
        }
    }
    fclose(f);
    printf("[%s] %d reads, %ld bytes, %d errs\n", a->name, reads, total, errs);
    return NULL;
}

int main(int argc, char **argv)
{
    if (argc > 1) g_ch = atoi(argv[1]);
    int secs = argc > 2 ? atoi(argv[2]) : 4;
    g_tone = argc > 3 ? atof(argv[3]) : 0.0;
    g_sweep = argc > 5 ? atoi(argv[5]) : 0;

    IOUSBDeviceInterface300 **dev = open_device();
    if (!dev) { fprintf(stderr, "device not found\n"); return 1; }
    (*dev)->USBDeviceOpen(dev); (*dev)->SetConfiguration(dev, 1);
    g_if0 = open_interface(dev, 0); g_if1 = open_interface(dev, 1);
    (*g_if0)->USBInterfaceOpen(g_if0); (*g_if1)->USBInterfaceOpen(g_if1);

    uint8_t fw[2] = {0}; vendor(dev, 0xC0, 0x56, 0, 0, fw, 2);
    (*g_if0)->SetAlternateInterface(g_if0, 1); (*g_if1)->SetAlternateInterface(g_if1, 1);
    uint8_t freq[3] = { RATE & 0xff, (RATE >> 8) & 0xff, (RATE >> 16) & 0xff };
    vendor(dev, 0x22, 0x01, 0x0100, 0x83, freq, 3); vendor(dev, 0x22, 0x01, 0x0100, 0x02, freq, 3);
    usleep(50000);
    uint8_t reg = 0; vendor(dev, 0xC0, 0x49, 0, 0, &reg, 1);
    vendor(dev, 0x40, 0x49, (reg & ~0x20) | 0x10, 0, NULL, 0);

    g_pipe_out = pipe_for(g_if0, 0x02); g_pipe_bulk_in = pipe_for(g_if0, 0x83);
    printf("fw %d.%d, pipe_out=%d bulk_in=%d, ch=%d tone=%.3f\n", fw[1], fw[0], g_pipe_out, g_pipe_bulk_in, g_ch, g_tone);

    for (int i = 0; i < NUM_XFERS; i++) {
        void *a, *f;
        (*g_if0)->LowLatencyCreateBuffer(g_if0, &a, ISOC_FRAMES * 8 * 3 * FRAMES_PER_PKT, kUSBLowLatencyWriteBuffer);
        (*g_if0)->LowLatencyCreateBuffer(g_if0, &f, ISOC_FRAMES * sizeof(IOUSBLowLatencyIsocFrame), kUSBLowLatencyFrameListBuffer);
        g_x[i].audio = a; g_x[i].frames = f;
    }
    CFRunLoopSourceRef src; (*g_if0)->CreateInterfaceAsyncEventSource(g_if0, &src);
    CFRunLoopAddSource(CFRunLoopGetCurrent(), src, kCFRunLoopDefaultMode);

    resync();
    if (!(argc > 4 && atoi(argv[4]))) for (int i = 0; i < NUM_XFERS; i++) submit(&g_x[i]);
    else printf("NOT starting isoc OUT stream\n");
    int pipe86 = pipe_for(g_if1, 0x86);
    RArg ra = { g_if0, g_pipe_bulk_in, "bulk83", "build/cap83.bin" };
    RArg rb = { g_if1, pipe86, "bulk86", "build/cap86.bin" };
    pthread_t th, th2; pthread_create(&th, NULL, reader, &ra); pthread_create(&th2, NULL, reader, &rb);

    CFAbsoluteTime end = CFAbsoluteTimeGetCurrent() + secs;
    while (CFAbsoluteTimeGetCurrent() < end) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.005, false);
    g_running = 0;
    pthread_join(th, NULL); pthread_join(th2, NULL);
    for (int i = 0; i < 5; i++) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, false);
    printf("isoc: done=%ld errs=%ld\n", g_done, g_errs);

    (*g_if0)->USBInterfaceClose(g_if0); (*g_if1)->USBInterfaceClose(g_if1); (*dev)->USBDeviceClose(dev);
    return 0;
}
