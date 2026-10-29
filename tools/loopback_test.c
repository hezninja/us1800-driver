/* loopback_test.c - measures the US-1800 through CoreAudio with output 1 patched into input 9.
 *   loopback_test rtl  [buffer_frames=64] [clicks=20]    round-trip latency, counted in frames of IO cycles
 *   loopback_test tone [seconds=60]                      1 kHz tone, reports pitch/level of the returned signal per 0.5 s
 * Build: clang -O2 -o build/loopback_test tools/loopback_test.c -framework CoreAudio -framework CoreFoundation -lm */
#include <CoreAudio/CoreAudio.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define IN_CH 10
#define OUT_CH 4
#define RX_SLOT 8                         /* input 9 */
#define RATE 48000.0

static int g_mode_rtl;
static UInt32 g_buf = 64;
static unsigned long g_cycle;             /* IO cycles so far */
static double g_ph;
/* rtl */
static long g_click_pos = -1;             /* absolute output frame index of the click in flight */
static double g_rtl[256]; static int g_nrtl, g_clicks_target = 20;
static unsigned long g_next_click = 200;
/* tone */
static float *g_rx; static unsigned long g_rx_n, g_rx_cap;

static OSStatus io(AudioObjectID d, const AudioTimeStamp *a, const AudioBufferList *in, const AudioTimeStamp *b,
                   AudioBufferList *out, const AudioTimeStamp *c, void *ref)
{
    UInt32 n = out && out->mNumberBuffers ? out->mBuffers[0].mDataByteSize / (4 * OUT_CH) : 0;
    float *o = out && out->mNumberBuffers ? out->mBuffers[0].mData : NULL;
    const float *x = in && in->mNumberBuffers ? in->mBuffers[0].mData : NULL;
    unsigned long base = g_cycle * g_buf;                 /* absolute frame index of the first frame of this cycle */
    if (g_mode_rtl) {
        if (x && g_click_pos >= 0) {                      /* look for the click coming back */
            for (UInt32 f = 0; f < n; f++) if (fabsf(x[f * IN_CH + RX_SLOT]) > 0.05f && (long)(base + f) > g_click_pos) {
                if (g_nrtl < 256) g_rtl[g_nrtl++] = (double)((long)(base + f) - g_click_pos);
                g_click_pos = -1; break;
            }
            if (g_click_pos >= 0 && (long)base - g_click_pos > 4800) g_click_pos = -1;      /* lost */
        }
        if (o) {
            memset(o, 0, n * OUT_CH * sizeof(float));
            if (g_click_pos < 0 && g_cycle >= g_next_click && g_nrtl < g_clicks_target) {
                o[0] = 0.3f; g_click_pos = (long)base; g_next_click = g_cycle + 4800 / g_buf;   /* one click every 100 ms */
            }
        }
    } else {
        if (o) for (UInt32 f = 0; f < n; f++) { for (int k = 0; k < OUT_CH; k++) o[f * OUT_CH + k] = 0;
            o[f * OUT_CH] = (float)(sin(g_ph) * 0.1); g_ph += 2 * M_PI * 1000.0 / RATE; }
        if (x) for (UInt32 f = 0; f < n && g_rx_n < g_rx_cap; f++) g_rx[g_rx_n++] = x[f * IN_CH + RX_SLOT];
    }
    g_cycle++;
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: see header\n"); return 1; }
    g_mode_rtl = !strcmp(argv[1], "rtl");
    double secs = 60;
    if (g_mode_rtl) { if (argc > 2) g_buf = (UInt32)atoi(argv[2]); if (argc > 3) g_clicks_target = atoi(argv[3]); }
    else if (argc > 2) secs = atof(argv[2]);
    if (!g_mode_rtl) { g_rx_cap = (unsigned long)((secs + 5) * RATE); g_rx = malloc(g_rx_cap * sizeof(float)); }

    CFStringRef uid = CFSTR("US1800-Userspace"); AudioObjectID dev = 0;
    AudioObjectPropertyAddress a = { kAudioHardwarePropertyTranslateUIDToDevice, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    UInt32 sz = sizeof dev;
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, sizeof uid, &uid, &sz, &dev) || !dev) { fprintf(stderr, "device not found\n"); return 1; }
    if (g_mode_rtl) {
        AudioObjectPropertyAddress bs = { kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
        OSStatus s = AudioObjectSetPropertyData(dev, &bs, 0, NULL, sizeof g_buf, &g_buf);
        UInt32 got = 0; sz = sizeof got; AudioObjectGetPropertyData(dev, &bs, 0, NULL, &sz, &got);
        printf("buffer size request %u -> %u frames (status %d)\n", g_buf, got, (int)s); g_buf = got;
    }
    AudioDeviceIOProcID id; OSStatus s = AudioDeviceCreateIOProcID(dev, io, NULL, &id);
    if (!s) s = AudioDeviceStart(dev, id);
    if (s) { fprintf(stderr, "start failed %d\n", (int)s); return 1; }
    double wait = g_mode_rtl ? 2.0 + g_clicks_target * 0.12 + 1 : secs;
    usleep((useconds_t)(wait * 1e6));
    AudioDeviceStop(dev, id); AudioDeviceDestroyIOProcID(dev, id);

    if (g_mode_rtl) {
        double mn = 1e9, mx = 0, sum = 0;
        for (int i = 0; i < g_nrtl; i++) { if (g_rtl[i] < mn) mn = g_rtl[i]; if (g_rtl[i] > mx) mx = g_rtl[i]; sum += g_rtl[i]; }
        if (!g_nrtl) { printf("no clicks came back (check the cable / levels)\n"); return 1; }
        printf("%d clicks: round trip min %.0f / mean %.1f / max %.0f frames = %.2f / %.2f / %.2f ms (at 48 kHz, includes the app's own %u-frame buffer)\n",
               g_nrtl, mn, sum / g_nrtl, mx, mn / 48, sum / g_nrtl / 48, mx / 48, g_buf);
    } else {
        printf("received %lu frames. 0.5 s blocks, judged against the run's own median level\n", g_rx_n);
        unsigned long blk = 24000, nb = g_rx_n > 48000 ? (g_rx_n - 24000) / blk : 0;
        double *rm = malloc((nb + 1) * sizeof(double)), *fq = malloc((nb + 1) * sizeof(double)), *sorted = malloc((nb + 1) * sizeof(double));
        for (unsigned long k = 0; k < nb; k++) {
            unsigned long s0 = 24000 + k * blk; double sq = 0; int zc = 0;
            for (unsigned long i = s0; i < s0 + blk; i++) { sq += g_rx[i] * g_rx[i]; if (g_rx[i - 1] < 0 && g_rx[i] >= 0) zc++; }
            rm[k] = sqrt(sq / blk); fq[k] = zc / (blk / RATE); sorted[k] = rm[k];
        }
        for (unsigned long i = 1; i < nb; i++) { double v = sorted[i]; long j = i - 1; while (j >= 0 && sorted[j] > v) { sorted[j + 1] = sorted[j]; j--; } sorted[j + 1] = v; }
        double med = nb ? sorted[nb / 2] : 0; int bad = 0;
        printf("median level %.3f rms\n", med);
        for (unsigned long k = 0; k < nb; k++) {
            int flag = fabs(fq[k] - 1000) > 6 || fabs(rm[k] - med) > 0.06 * med;
            if (flag) { bad++; if (bad <= 25) printf("  t=%6.1fs rms=%.3f freq=%7.1f Hz <-- ANOMALY\n", (24000 + k * blk) / RATE, rm[k], fq[k]); }
        }
        printf("anomalous blocks: %d of %lu\n", bad, nb);
    }
    return 0;
}
