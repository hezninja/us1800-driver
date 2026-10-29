/* hal_test.c - opens the US-1800 HAL device by UID, runs IO for N seconds, prints per-channel input RMS.
 * Optional: plays a 440 Hz tone on output channel <ch> (1..4) at amplitude <amp>.
 * Usage: hal_test <seconds> [out_ch amp]    Build: clang -o build/hal_test tools/hal_test.c -framework CoreAudio -framework CoreFoundation -lm */
#include <CoreAudio/CoreAudio.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#define IN_CH 10
#define OUT_CH 4
static double g_sq[IN_CH]; static unsigned long g_n, g_calls; static int g_ch = -1; static double g_amp, g_ph;
static OSStatus io(AudioObjectID d, const AudioTimeStamp *a, const AudioBufferList *in, const AudioTimeStamp *b,
                   AudioBufferList *out, const AudioTimeStamp *c, void *ref)
{
    g_calls++;
    if (in && in->mNumberBuffers) { const float *p = in->mBuffers[0].mData; UInt32 n = in->mBuffers[0].mDataByteSize / (4 * IN_CH);
        for (UInt32 f = 0; f < n; f++) for (int k = 0; k < IN_CH; k++) g_sq[k] += (double)p[f*IN_CH+k] * p[f*IN_CH+k]; g_n += n; }
    if (out && out->mNumberBuffers) { float *p = out->mBuffers[0].mData; UInt32 n = out->mBuffers[0].mDataByteSize / (4 * OUT_CH);
        for (UInt32 f = 0; f < n; f++) { for (int k = 0; k < OUT_CH; k++) p[f*OUT_CH+k] = 0;
            if (g_ch >= 0) { p[f*OUT_CH+g_ch] = (float)(sin(g_ph) * g_amp); g_ph += 2*M_PI*440.0/48000.0; } } }
    return 0;
}
int main(int argc, char **argv)
{
    double secs = argc > 1 ? atof(argv[1]) : 3;
    if (argc > 3) { g_ch = atoi(argv[2]) - 1; g_amp = atof(argv[3]); }
    CFStringRef uid = CFSTR("US1800-Userspace"); AudioObjectID dev = 0;
    AudioObjectPropertyAddress a = { kAudioHardwarePropertyTranslateUIDToDevice, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    UInt32 sz = sizeof dev;
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, sizeof uid, &uid, &sz, &dev) || !dev) { fprintf(stderr, "device not found\n"); return 1; }
    AudioDeviceIOProcID id; OSStatus s = AudioDeviceCreateIOProcID(dev, io, NULL, &id);
    if (!s) s = AudioDeviceStart(dev, id);
    if (s) { fprintf(stderr, "start failed %d\n", (int)s); return 1; }
    usleep((useconds_t)(secs * 1e6));
    AudioDeviceStop(dev, id); AudioDeviceDestroyIOProcID(dev, id);
    printf("io calls=%lu frames=%lu\nrms:", g_calls, g_n);
    for (int k = 0; k < IN_CH; k++) printf(" %.1e", g_n ? sqrt(g_sq[k] / g_n) : 0);
    printf("\n"); return 0;
}
