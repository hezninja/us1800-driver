/*
 * resample.h - fractional-delay interpolator for the plug-in's drift correction.
 *
 * 32-tap Kaiser-windowed sinc, 1024 fractional phases. Linear interpolation is not good enough here: its gain at high
 * frequencies depends on the fractional position, and while the position slowly sweeps (drift correction) that is heard as
 * a swishy modulation of the treble. This filter is flat to ~20 kHz at every position.
 */
#pragma once
#include <math.h>
#include <stdint.h>

#define RS_TAPS    32
#define RS_BEFORE  15            /* taps before the sample at floor(position); RS_TAPS - 1 - RS_BEFORE after it */
#define RS_AFTER   16
#define RS_PHASES  1024

static float gRsTable[RS_PHASES][RS_TAPS];

static double rs_bessel_i0(double x)
{
    double sum = 1, term = 1;
    for (int k = 1; k < 40; k++) { term *= (x / (2.0 * k)) * (x / (2.0 * k)); sum += term; }
    return sum;
}

static void rs_init(void)
{
    const double cut = 0.97, beta = 9.0, half = 16.0;
    const double norm = rs_bessel_i0(beta);
    for (int p = 0; p < RS_PHASES; p++) {
        double frac = (double)p / RS_PHASES, sum = 0, h[RS_TAPS];
        for (int k = 0; k < RS_TAPS; k++) {
            double t = (double)(k - RS_BEFORE) - frac;              /* distance from the wanted position to this tap */
            double x = t / half;
            double w = fabs(x) < 1 ? rs_bessel_i0(beta * sqrt(1 - x * x)) / norm : 0;
            double s = fabs(t) < 1e-12 ? 1.0 : sin(M_PI * cut * t) / (M_PI * cut * t);
            h[k] = s * w; sum += h[k];
        }
        for (int k = 0; k < RS_TAPS; k++) gRsTable[p][k] = (float)(h[k] / sum);   /* unity gain at DC for every phase */
    }
}

/* Splits a position into the integer sample index and the table row; handles rounding up to the next sample. */
static inline const float *rs_taps(double pos, int64_t *index)
{
    int64_t i = (int64_t)floor(pos);
    int p = (int)((pos - (double)i) * RS_PHASES + 0.5);
    if (p >= RS_PHASES) { p = 0; i++; }
    *index = i;
    return gRsTable[p];
}
