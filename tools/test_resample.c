/* test_resample.c - offline check of plugin/resample.h against linear interpolation.
 * Resamples sines through a slowly sweeping fractional position and reports the error vs the ideal sine.
 * Build: clang -O2 -o build/test_resample tools/test_resample.c -lm */
#include <stdio.h>
#include "../plugin/resample.h"

#define N 20000
int main(void)
{
    rs_init();
    static double x[N];
    double freqs[] = { 1000, 5000, 10000, 15000, 18000, 20000 };
    printf("error of the interpolated sine vs the ideal one, dB relative to the signal (more negative is better)\n");
    printf("%8s  %-12s %-12s %-14s\n", "freq", "sinc32", "linear", "level (sinc)");
    for (int fi = 0; fi < 6; fi++) {
        double f = freqs[fi];
        for (int n = 0; n < N; n++) x[n] = sin(2 * M_PI * f * n / 48000.0);
        double e1 = 0, e2 = 0, s = 0, lvmin = 1e9, lvmax = 0;
        double pos = 100.25;
        for (int o = 0; o < 16000; o++) {                           /* step 1.0005: fraction sweeps through 0..1 every 2000 frames */
            int64_t i; const float *t = rs_taps(pos, &i);
            double y = 0; for (int k = 0; k < RS_TAPS; k++) y += t[k] * x[i - RS_BEFORE + k];
            double ideal = sin(2 * M_PI * f * pos / 48000.0);
            double lin = x[(int64_t)floor(pos)] + (x[(int64_t)floor(pos) + 1] - x[(int64_t)floor(pos)]) * (pos - floor(pos));
            e1 += (y - ideal) * (y - ideal); e2 += (lin - ideal) * (lin - ideal); s += ideal * ideal;
            pos += 1.0005;
        }
        printf("%7.0fHz  %8.1f dB  %8.1f dB\n", f, 10 * log10(e1 / s + 1e-30), 10 * log10(e2 / s + 1e-30));
    }
    /* gain flatness of the filter itself at a few fractional delays */
    printf("\ngain of the 32-tap filter (dB) at fractional delay 0 / 0.25 / 0.5 / 0.75:\n");
    for (int fi = 0; fi < 6; fi++) {
        printf("%7.0fHz ", freqs[fi]);
        for (int q = 0; q < 4; q++) {
            int p = q * RS_PHASES / 4; double re = 0, im = 0, w = 2 * M_PI * freqs[fi] / 48000.0;
            for (int k = 0; k < RS_TAPS; k++) { re += gRsTable[p][k] * cos(w * (k - RS_BEFORE)); im += gRsTable[p][k] * sin(w * (k - RS_BEFORE)); }
            printf(" %7.3f", 20 * log10(sqrt(re * re + im * im)));
        }
        printf("\n");
    }
    return 0;
}
