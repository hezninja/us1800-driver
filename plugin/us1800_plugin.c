/*
 * us1800_plugin.c - CoreAudio AudioServerPlugIn (HAL plug-in) for the TASCAM US-1800.
 *
 * The plug-in does no USB I/O itself: it exchanges audio with us1800d through the shared-memory rings in
 * us1800_shm.h. Fixed format: 48 kHz, Float32, 14 inputs (analog inputs 1-14) and 4 outputs.
 *
 * Build: see plugin/build.sh
 */
#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <errno.h>
#include <fcntl.h>
#include <mach/mach_time.h>
#include <math.h>
#include <os/log.h>
#include <pthread.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "../src/us1800_shm.h"
#include "resample.h"

#define PLUGIN_ID       kAudioObjectPlugInObject    /* 1 */
#define DEVICE_ID       2
#define STREAM_IN_ID    3
#define STREAM_OUT_ID   4

#define RATE            48000.0
#define IN_CHANNELS     14
#define OUT_CHANNELS    4
#define ZTS_PERIOD      4096

static os_log_t gLog;
static pthread_mutex_t gMutex = PTHREAD_MUTEX_INITIALIZER;
static UInt32 gIOClients;
static UInt64 gAnchorHost;
static Float64 gHostTicksPerFrame;
static us1800_shm *gShm;
static double gCapPos;                  /* absolute capture-ring position of the next output frame (fractional) */
static double gCapErr, gPlayErr;        /* smoothed fill error in frames */
static double gPlayU;                   /* position of the next ring frame within the HAL stream; -1..0 means "between blocks" */
#define MAX_BLOCK 16384
static float gPlayWork[(MAX_BLOCK + RS_TAPS) * OUT_CHANNELS];     /* previous block's tail followed by the current block */
static uint64_t gPlayW;                 /* next ring index to write */
static int gCapInit, gPlayInit;

/* ---- shared memory ----------------------------------------------------------------------------------------- */

static void shm_attach(void)
{
    if (gShm) { munmap(gShm, sizeof *gShm); gShm = NULL; }
    int fd = shm_open(US1800_SHM_NAME, O_RDWR, 0);
    if (fd < 0) { os_log_error(gLog, "us1800: shm_open failed, errno %d", errno); return; }
    void *p = mmap(NULL, sizeof(us1800_shm), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) { os_log_error(gLog, "us1800: mmap failed, errno %d", errno); return; }
    if (((us1800_shm *)p)->magic != US1800_SHM_MAGIC) {
        os_log_error(gLog, "us1800: bad shm magic");
        munmap(p, sizeof(us1800_shm));
        return;
    }
    gShm = p;
    os_log(gLog, "us1800: attached to daemon shm");
}

/* ---- COM plumbing ------------------------------------------------------------------------------------------ */

static HRESULT QueryInterface(void *drv, REFIID uuid, LPVOID *out);
static ULONG AddRef(void *drv) { (void)drv; return 1; }
static ULONG Release(void *drv) { (void)drv; return 1; }
static OSStatus Initialize(AudioServerPlugInDriverRef drv, AudioServerPlugInHostRef host);
static OSStatus CreateDevice(AudioServerPlugInDriverRef d, CFDictionaryRef desc, const AudioServerPlugInClientInfo *ci, AudioObjectID *o)
{ (void)d; (void)desc; (void)ci; (void)o; return kAudioHardwareUnsupportedOperationError; }
static OSStatus DestroyDevice(AudioServerPlugInDriverRef d, AudioObjectID o)
{ (void)d; (void)o; return kAudioHardwareUnsupportedOperationError; }
static OSStatus AddDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID o, const AudioServerPlugInClientInfo *ci)
{ (void)d; (void)o; (void)ci; return 0; }
static OSStatus RemoveDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID o, const AudioServerPlugInClientInfo *ci)
{ (void)d; (void)o; (void)ci; return 0; }
static OSStatus PerformDeviceConfigurationChange(AudioServerPlugInDriverRef d, AudioObjectID o, UInt64 a, void *i)
{ (void)d; (void)o; (void)a; (void)i; return 0; }
static OSStatus AbortDeviceConfigurationChange(AudioServerPlugInDriverRef d, AudioObjectID o, UInt64 a, void *i)
{ (void)d; (void)o; (void)a; (void)i; return 0; }

/* ---- properties -------------------------------------------------------------------------------------------- */

static AudioStreamBasicDescription stream_format(UInt32 channels)
{
    AudioStreamBasicDescription f = {0};
    f.mSampleRate = RATE;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked;
    f.mBitsPerChannel = 32;
    f.mChannelsPerFrame = channels;
    f.mBytesPerFrame = 4 * channels;
    f.mFramesPerPacket = 1;
    f.mBytesPerPacket = f.mBytesPerFrame;
    return f;
}

#define PUT(type, val) do { size = sizeof(type); \
    if (out) { if (inSize < sizeof(type)) return kAudioHardwareBadPropertySizeError; *(type *)out = (val); } } while (0)

static OSStatus put_ids(const AudioObjectID *ids, UInt32 n, UInt32 inSize, UInt32 *outSize, void *out)
{
    if (out) { UInt32 c = inSize / sizeof(AudioObjectID); if (c > n) c = n; memcpy(out, ids, c * sizeof *ids); n = c; }
    *outSize = n * sizeof(AudioObjectID);
    return 0;
}

/* One routine answers HasProperty / GetPropertyDataSize / GetPropertyData: out == NULL means "size only". */
static OSStatus get_prop(AudioObjectID obj, const AudioObjectPropertyAddress *a, UInt32 qualSize, const void *qual,
                         UInt32 inSize, UInt32 *outSize, void *out)
{
    UInt32 size = 0;
    AudioObjectPropertyScope sc = a->mScope;

    /* properties common to every object */
    switch (a->mSelector) {
    case kAudioObjectPropertyBaseClass:
        if (obj == PLUGIN_ID || obj == DEVICE_ID || obj == STREAM_IN_ID || obj == STREAM_OUT_ID) {
            PUT(AudioClassID, kAudioObjectClassID); *outSize = size; return 0;
        }
        break;
    case kAudioObjectPropertyClass:
        PUT(AudioClassID, obj == PLUGIN_ID ? kAudioPlugInClassID : obj == DEVICE_ID ? kAudioDeviceClassID : kAudioStreamClassID);
        *outSize = size; return 0;
    case kAudioObjectPropertyOwner:
        PUT(AudioObjectID, obj == PLUGIN_ID ? kAudioObjectUnknown : obj == DEVICE_ID ? PLUGIN_ID : DEVICE_ID);
        *outSize = size; return 0;
    default: break;
    }

    if (obj == PLUGIN_ID) {
        static const AudioObjectID dev = DEVICE_ID;
        switch (a->mSelector) {
        case kAudioObjectPropertyManufacturer: PUT(CFStringRef, CFSTR("us1800 userspace driver")); break;
        case kAudioObjectPropertyOwnedObjects:
        case kAudioPlugInPropertyDeviceList: return put_ids(&dev, 1, inSize, outSize, out);
        case kAudioPlugInPropertyTranslateUIDToDevice: {
            AudioObjectID r = kAudioObjectUnknown;
            if (qualSize == sizeof(CFStringRef) && qual &&
                CFStringCompare(*(CFStringRef *)qual, CFSTR("US1800-Userspace"), 0) == kCFCompareEqualTo) r = DEVICE_ID;
            PUT(AudioObjectID, r); break;
        }
        case kAudioPlugInPropertyResourceBundle: PUT(CFStringRef, CFSTR("")); break;
        default: return kAudioHardwareUnknownPropertyError;
        }
        *outSize = size; return 0;
    }

    if (obj == DEVICE_ID) {
        static const AudioObjectID both[2] = { STREAM_IN_ID, STREAM_OUT_ID };
        static const AudioObjectID sin = STREAM_IN_ID, sout = STREAM_OUT_ID;
        switch (a->mSelector) {
        case kAudioObjectPropertyName: PUT(CFStringRef, CFSTR("TASCAM US-1800")); break;
        case kAudioObjectPropertyManufacturer: PUT(CFStringRef, CFSTR("TASCAM")); break;
        case kAudioObjectPropertyModelName: PUT(CFStringRef, CFSTR("US-1800")); break;
        case kAudioDevicePropertyDeviceUID: PUT(CFStringRef, CFSTR("US1800-Userspace")); break;
        case kAudioDevicePropertyModelUID: PUT(CFStringRef, CFSTR("US1800-Userspace-Model")); break;
        case kAudioDevicePropertyTransportType: PUT(UInt32, kAudioDeviceTransportTypeUSB); break;
        case kAudioDevicePropertyRelatedDevices: { static const AudioObjectID me = DEVICE_ID; return put_ids(&me, 1, inSize, outSize, out); }
        case kAudioDevicePropertyClockDomain: PUT(UInt32, 0); break;
        case kAudioDevicePropertyDeviceIsAlive: PUT(UInt32, 1); break;
        case kAudioDevicePropertyDeviceIsRunning: PUT(UInt32, gIOClients > 0); break;
        case kAudioDevicePropertyDeviceCanBeDefaultDevice: PUT(UInt32, 1); break;
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice: PUT(UInt32, 1); break;
        case kAudioDevicePropertyLatency:
            PUT(UInt32, sc == kAudioObjectPropertyScopeInput ? US1800_IN_LATENCY : sc == kAudioObjectPropertyScopeOutput ? US1800_OUT_LATENCY : 0);
            break;
        case kAudioDevicePropertySafetyOffset: PUT(UInt32, 0); break;
        case kAudioDevicePropertyStreams:
            if (sc == kAudioObjectPropertyScopeInput) return put_ids(&sin, 1, inSize, outSize, out);
            if (sc == kAudioObjectPropertyScopeOutput) return put_ids(&sout, 1, inSize, outSize, out);
            return put_ids(both, 2, inSize, outSize, out);
        case kAudioObjectPropertyOwnedObjects: return put_ids(both, 2, inSize, outSize, out);
        case kAudioObjectPropertyControlList: size = 0; break;
        case kAudioDevicePropertyNominalSampleRate: PUT(Float64, RATE); break;
        case kAudioDevicePropertyAvailableNominalSampleRates: {
            AudioValueRange r = { RATE, RATE }; PUT(AudioValueRange, r); break;
        }
        case kAudioDevicePropertyIsHidden: PUT(UInt32, 0); break;
        case kAudioDevicePropertyZeroTimeStampPeriod: PUT(UInt32, ZTS_PERIOD); break;
        default: return kAudioHardwareUnknownPropertyError;
        }
        *outSize = size; return 0;
    }

    if (obj == STREAM_IN_ID || obj == STREAM_OUT_ID) {
        UInt32 ch = obj == STREAM_IN_ID ? IN_CHANNELS : OUT_CHANNELS;
        switch (a->mSelector) {
        case kAudioStreamPropertyIsActive: PUT(UInt32, 1); break;
        case kAudioStreamPropertyDirection: PUT(UInt32, obj == STREAM_IN_ID ? 1 : 0); break;
        case kAudioStreamPropertyTerminalType: PUT(UInt32, obj == STREAM_IN_ID ? kAudioStreamTerminalTypeLine : kAudioStreamTerminalTypeLine); break;
        case kAudioStreamPropertyStartingChannel: PUT(UInt32, 1); break;
        case kAudioStreamPropertyLatency: PUT(UInt32, 0); break;
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat: { AudioStreamBasicDescription f = stream_format(ch); PUT(AudioStreamBasicDescription, f); break; }
        case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyAvailablePhysicalFormats: {
            AudioStreamRangedDescription f = { stream_format(ch), { RATE, RATE } };
            PUT(AudioStreamRangedDescription, f); break;
        }
        default: return kAudioHardwareUnknownPropertyError;
        }
        *outSize = size; return 0;
    }
    return kAudioHardwareBadObjectError;
}

static Boolean HasProperty(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid, const AudioObjectPropertyAddress *a)
{
    (void)d; (void)pid; UInt32 sz;
    return get_prop(o, a, 0, NULL, 0xFFFFFFFF, &sz, NULL) == 0;
}

static OSStatus IsPropertySettable(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid, const AudioObjectPropertyAddress *a, Boolean *settable)
{
    (void)d; (void)pid; UInt32 sz;
    OSStatus s = get_prop(o, a, 0, NULL, 0xFFFFFFFF, &sz, NULL);
    if (s) return s;
    *settable = (o == DEVICE_ID && a->mSelector == kAudioDevicePropertyNominalSampleRate);
    return 0;
}

static OSStatus GetPropertyDataSize(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid, const AudioObjectPropertyAddress *a,
                                    UInt32 qs, const void *q, UInt32 *outSize)
{
    (void)d; (void)pid;
    return get_prop(o, a, qs, q, 0xFFFFFFFF, outSize, NULL);
}

static OSStatus GetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid, const AudioObjectPropertyAddress *a,
                                UInt32 qs, const void *q, UInt32 inSize, UInt32 *outSize, void *out)
{
    (void)d; (void)pid;
    return get_prop(o, a, qs, q, inSize, outSize, out);
}

static OSStatus SetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid, const AudioObjectPropertyAddress *a,
                                UInt32 qs, const void *q, UInt32 inSize, const void *in)
{
    (void)d; (void)pid; (void)qs; (void)q;
    if (o == DEVICE_ID && a->mSelector == kAudioDevicePropertyNominalSampleRate) {
        if (inSize != sizeof(Float64)) return kAudioHardwareBadPropertySizeError;
        return *(const Float64 *)in == RATE ? 0 : kAudioDeviceUnsupportedFormatError;
    }
    return kAudioHardwareUnsupportedOperationError;
}

/* ---- IO ---------------------------------------------------------------------------------------------------- */

static OSStatus StartIO(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client)
{
    (void)d; (void)client;
    if (dev != DEVICE_ID) return kAudioHardwareBadObjectError;
    pthread_mutex_lock(&gMutex);
    if (gIOClients == 0) {
        mach_timebase_info_data_t tb; mach_timebase_info(&tb);
        gHostTicksPerFrame = (1e9 / RATE) * ((Float64)tb.denom / (Float64)tb.numer);
        gAnchorHost = mach_absolute_time();
        shm_attach();
        gCapInit = gPlayInit = 0;
    }
    gIOClients++;
    pthread_mutex_unlock(&gMutex);
    return 0;
}

static OSStatus StopIO(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client)
{
    (void)d; (void)client;
    if (dev != DEVICE_ID) return kAudioHardwareBadObjectError;
    pthread_mutex_lock(&gMutex);
    if (gIOClients > 0) gIOClients--;
    pthread_mutex_unlock(&gMutex);
    return 0;
}

static OSStatus GetZeroTimeStamp(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client, Float64 *st, UInt64 *ht, UInt64 *seed)
{
    (void)d; (void)dev; (void)client;
    Float64 ticksPerPeriod = gHostTicksPerFrame * ZTS_PERIOD;
    UInt64 periods = (UInt64)((mach_absolute_time() - gAnchorHost) / ticksPerPeriod);
    *st = (Float64)(periods * ZTS_PERIOD);
    *ht = gAnchorHost + (UInt64)(periods * ticksPerPeriod);
    *seed = 1;
    return 0;
}

static OSStatus WillDoIOOperation(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client, UInt32 op, Boolean *will, Boolean *inPlace)
{
    (void)d; (void)dev; (void)client;
    *will = (op == kAudioServerPlugInIOOperationReadInput || op == kAudioServerPlugInIOOperationWriteMix);
    *inPlace = true;
    return 0;
}

static OSStatus BeginIOOperation(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client, UInt32 op, UInt32 n, const AudioServerPlugInIOCycleInfo *i)
{ (void)d; (void)dev; (void)client; (void)op; (void)n; (void)i; return 0; }
static OSStatus EndIOOperation(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client, UInt32 op, UInt32 n, const AudioServerPlugInIOCycleInfo *i)
{ (void)d; (void)dev; (void)client; (void)op; (void)n; (void)i; return 0; }

/* The device, the Mac and the USB frame clock all run a few tens of ppm apart, so a ring filled by one and drained by another
 * slowly creeps. Instead of letting it hit the end and jumping (a click), each block is read or written through a linear
 * resampler whose ratio is nudged by at most 0.1% to hold the ring at its target fill. ~5 s time constant, inaudible. */
#define RATE_TAU_FRAMES 240000.0        /* 5 s at 48 kHz */
#define MAX_RATE_ADJ    0.001


static double rate_adj(double *smoothed, double err, UInt32 n)
{
    *smoothed += (err - *smoothed) * ((double)n / RATE);       /* ~1 s smoothing of the fill error */
    double d = *smoothed / RATE_TAU_FRAMES;
    return d > MAX_RATE_ADJ ? MAX_RATE_ADJ : d < -MAX_RATE_ADJ ? -MAX_RATE_ADJ : d;
}

static OSStatus DoIOOperation(AudioServerPlugInDriverRef d, AudioObjectID dev, AudioObjectID stream, UInt32 client, UInt32 op,
                              UInt32 n, const AudioServerPlugInIOCycleInfo *info, void *main, void *secondary)
{
    (void)d; (void)dev; (void)client; (void)info; (void)secondary;
    const uint64_t mask = US1800_RING_FRAMES - 1;
    float *buf = main;

    if (op == kAudioServerPlugInIOOperationReadInput && stream == STREAM_IN_ID) {
        if (!gShm || !atomic_load_explicit(&gShm->running, memory_order_relaxed)) {      /* no daemon, or unit unplugged */
            memset(buf, 0, n * IN_CHANNELS * sizeof(float)); gCapInit = 0; return 0; }
        const uint32_t cushion = gShm->cap_cushion;
        uint64_t w = atomic_load_explicit(&gShm->cap_w, memory_order_acquire);
        double avail = (double)w - gCapPos;
        if (!gCapInit || avail < n * 1.002 + RS_AFTER + 2 || avail > US1800_RING_FRAMES - 4096) {   /* first read, or the ring really ran dry/full */
            if (gCapInit) { atomic_fetch_add(&gShm->plug_resyncs, 1); os_log(gLog, "us1800: capture resync avail=%.0f n=%u", avail, (unsigned)n); }
            gCapPos = w > n + cushion ? (double)(w - n - cushion) : 0;
            gCapErr = 0; gCapInit = 1;
            avail = (double)w - gCapPos;
        }
        double step = 1.0 + rate_adj(&gCapErr, avail - (double)(n + cushion), n);
        for (UInt32 f = 0; f < n; f++) {
            int64_t i; const float *t = rs_taps(gCapPos, &i);
            float acc[IN_CHANNELS] = {0};
            for (int k = 0; k < RS_TAPS; k++) {
                const int32_t *row = gShm->cap[(uint64_t)(i - RS_BEFORE + k) & mask];
                for (int c = 0; c < IN_CHANNELS; c++) acc[c] += t[k] * (float)row[c];
            }
            for (int c = 0; c < IN_CHANNELS; c++) buf[f * IN_CHANNELS + c] = acc[c] * (1.0f / 2147483648.0f);
            gCapPos += step;
        }
        return 0;
    }

    if (op == kAudioServerPlugInIOOperationWriteMix && stream == STREAM_OUT_ID) {
        if (!gShm || n > MAX_BLOCK) return 0;
        const uint32_t prefill = gShm->play_prefill;
        uint64_t r = atomic_load_explicit(&gShm->play_r, memory_order_acquire);         /* the daemon only advances this on real frames */
        double fill = (double)(gPlayW - r);
        if (!gPlayInit || gPlayW <= r || fill > US1800_RING_FRAMES - 4096) {            /* first write, or the daemon ran dry/ring full */
            if (gPlayInit) { atomic_fetch_add(&gShm->plug_resyncs, 1); os_log(gLog, "us1800: playback resync fill=%.0f", fill); }
            gPlayW = r;
            for (uint64_t i = 0; i < prefill; i++) memset(gShm->play[(gPlayW + i) & mask], 0, sizeof gShm->play[0]);
            gPlayW += prefill;
            gPlayErr = 0; gPlayU = 0; memset(gPlayWork, 0, RS_TAPS * OUT_CHANNELS * sizeof(float)); gPlayInit = 1;
            fill = (double)prefill;
        }
        double step = 1.0 + rate_adj(&gPlayErr, fill - (double)prefill, n);              /* ring too full -> consume the input faster */
        memcpy(gPlayWork + RS_TAPS * OUT_CHANNELS, buf, n * OUT_CHANNELS * sizeof(float));
        const float *blk = gPlayWork + RS_TAPS * OUT_CHANNELS;                           /* blk[j] = frame j of this HAL block, j may be negative */
        double u = gPlayU;
        while ((int64_t)floor(u) + RS_AFTER < (int64_t)n) {
            int64_t i; const float *t = rs_taps(u, &i);
            int32_t *dst = gShm->play[gPlayW & mask];
            float acc[OUT_CHANNELS] = {0};
            for (int k = 0; k < RS_TAPS; k++) {
                const float *row = blk + (i - RS_BEFORE + k) * OUT_CHANNELS;
                for (int c = 0; c < OUT_CHANNELS; c++) acc[c] += t[k] * row[c];
            }
            for (int c = 0; c < OUT_CHANNELS; c++) {
                float x = acc[c] > 1.0f ? 1.0f : acc[c] < -1.0f ? -1.0f : acc[c];
                dst[c] = (int32_t)lrintf(x * 8388607.0f) * 256;
            }
            for (int c = OUT_CHANNELS; c < US1800_MAX_OUT; c++) dst[c] = 0;
            gPlayW++; u += step;
        }
        gPlayU = u - (double)n;
        memmove(gPlayWork, gPlayWork + n * OUT_CHANNELS, RS_TAPS * OUT_CHANNELS * sizeof(float));   /* keep the last 32 frames as history */
        atomic_store_explicit(&gShm->play_w, gPlayW, memory_order_release);
        return 0;
    }
    return 0;
}

/* ---- driver entry ------------------------------------------------------------------------------------------ */

static AudioServerPlugInDriverInterface gInterface = {
    NULL, QueryInterface, AddRef, Release, Initialize, CreateDevice, DestroyDevice, AddDeviceClient, RemoveDeviceClient,
    PerformDeviceConfigurationChange, AbortDeviceConfigurationChange, HasProperty, IsPropertySettable, GetPropertyDataSize,
    GetPropertyData, SetPropertyData, StartIO, StopIO, GetZeroTimeStamp, WillDoIOOperation, BeginIOOperation, DoIOOperation,
    EndIOOperation
};
static AudioServerPlugInDriverInterface *gInterfacePtr = &gInterface;
static AudioServerPlugInDriverRef gDriverRef = &gInterfacePtr;

static HRESULT QueryInterface(void *drv, REFIID uuid, LPVOID *out)
{
    (void)drv;
    CFUUIDRef req = CFUUIDCreateFromUUIDBytes(NULL, uuid);
    HRESULT r = E_NOINTERFACE;
    if (CFEqual(req, IUnknownUUID) || CFEqual(req, kAudioServerPlugInDriverInterfaceUUID)) { *out = gDriverRef; r = S_OK; }
    CFRelease(req);
    return r;
}

static OSStatus Initialize(AudioServerPlugInDriverRef drv, AudioServerPlugInHostRef host)
{
    (void)drv; (void)host;
    os_log(gLog, "us1800: plug-in initialised");
    return 0;
}

void *US1800_Create(CFAllocatorRef alloc, CFUUIDRef typeUUID)
{
    (void)alloc;
    if (!CFEqual(typeUUID, kAudioServerPlugInTypeUUID)) return NULL;
    gLog = os_log_create("local.us1800", "plugin");
    rs_init();
    return gDriverRef;
}
