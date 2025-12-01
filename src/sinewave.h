/*
 * Sine waveform implementation
 *
 * Configuration (define before including picosynth.h):
 *   PICOSYNTH_SINE_LUT_8BIT  - 8-bit 129-entry LUT (default, smallest)
 *   PICOSYNTH_SINE_LUT_16BIT - 16-bit 257-entry LUT (higher quality)
 *   PICOSYNTH_USE_SINF       - Use sinf() (highest quality, needs FPU)
 */

#ifndef PICOSYNTH_MATH_H_
#define PICOSYNTH_MATH_H_

#include "picosynth.h"

/* Default to 8-bit LUT if no mode specified */
#if !defined(PICOSYNTH_SINE_LUT_8BIT) && !defined(PICOSYNTH_SINE_LUT_16BIT) && \
    !defined(PICOSYNTH_USE_SINF)
#define PICOSYNTH_SINE_LUT_8BIT 1
#endif

#ifndef PICOSYNTH_INTERPOLATE
#define PICOSYNTH_INTERPOLATE 1
#endif

#ifdef PICOSYNTH_USE_SINF
#include <math.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#endif

/* Sine LUTs: quarter-cycle with extra entry for interpolation */
#if defined(PICOSYNTH_SINE_LUT_8BIT) && PICOSYNTH_SINE_LUT_8BIT
static const q7_t sine_lut8[129] = {
    0,    6,    12,   19,   25,   31,   37,   43,   49,   54,   60,   65,
    71,   76,   81,   85,   90,   94,   98,   102,  106,  109,  112,  115,
    117,  120,  122,  123,  125,  126,  126,  127,  127,  127,  126,  126,
    125,  123,  122,  120,  117,  115,  112,  109,  106,  102,  98,   94,
    90,   85,   81,   76,   71,   65,   60,   54,   49,   43,   37,   31,
    25,   19,   12,   6,    0,    -6,   -12,  -19,  -25,  -31,  -37,  -43,
    -49,  -54,  -60,  -65,  -71,  -76,  -81,  -85,  -90,  -94,  -98,  -102,
    -106, -109, -112, -115, -117, -120, -122, -123, -125, -126, -126, -127,
    -127, -127, -126, -126, -125, -123, -122, -120, -117, -115, -112, -109,
    -106, -102, -98,  -94,  -90,  -85,  -81,  -76,  -71,  -65,  -60,  -54,
    -49,  -43,  -37,  -31,  -25,  -19,  -12,  -6,   0,
};
#elif defined(PICOSYNTH_SINE_LUT_16BIT) && PICOSYNTH_SINE_LUT_16BIT
static const q15_t sine_lut16[257] = {
    0,      804,    1608,   2410,   3212,   4011,   4808,   5602,   6393,
    7179,   7962,   8739,   9512,   10278,  11039,  11793,  12539,  13279,
    14010,  14732,  15446,  16151,  16846,  17530,  18204,  18868,  19519,
    20159,  20787,  21403,  22005,  22594,  23170,  23731,  24279,  24811,
    25329,  25832,  26319,  26790,  27245,  27683,  28105,  28510,  28898,
    29268,  29621,  29956,  30273,  30571,  30852,  31113,  31356,  31580,
    31785,  31971,  32137,  32285,  32412,  32521,  32609,  32678,  32728,
    32757,  32767,  32757,  32728,  32678,  32609,  32521,  32412,  32285,
    32137,  31971,  31785,  31580,  31356,  31113,  30852,  30571,  30273,
    29956,  29621,  29268,  28898,  28510,  28105,  27683,  27245,  26790,
    26319,  25832,  25329,  24811,  24279,  23731,  23170,  22594,  22005,
    21403,  20787,  20159,  19519,  18868,  18204,  17530,  16846,  16151,
    15446,  14732,  14010,  13279,  12539,  11793,  11039,  10278,  9512,
    8739,   7962,   7179,   6393,   5602,   4808,   4011,   3212,   2410,
    1608,   804,    0,      -804,   -1608,  -2410,  -3212,  -4011,  -4808,
    -5602,  -6393,  -7179,  -7962,  -8739,  -9512,  -10278, -11039, -11793,
    -12539, -13279, -14010, -14732, -15446, -16151, -16846, -17530, -18204,
    -18868, -19519, -20159, -20787, -21403, -22005, -22594, -23170, -23731,
    -24279, -24811, -25329, -25832, -26319, -26790, -27245, -27683, -28105,
    -28510, -28898, -29268, -29621, -29956, -30273, -30571, -30852, -31113,
    -31356, -31580, -31785, -31971, -32137, -32285, -32412, -32521, -32609,
    -32678, -32728, -32757, -32767, -32757, -32728, -32678, -32609, -32521,
    -32412, -32285, -32137, -31971, -31785, -31580, -31356, -31113, -30852,
    -30571, -30273, -29956, -29621, -29268, -28898, -28510, -28105, -27683,
    -27245, -26790, -26319, -25832, -25329, -24811, -24279, -23731, -23170,
    -22594, -22005, -21403, -20787, -20159, -19519, -18868, -18204, -17530,
    -16846, -16151, -15446, -14732, -14010, -13279, -12539, -11793, -11039,
    -10278, -9512,  -8739,  -7962,  -7179,  -6393,  -5602,  -4808,  -4011,
    -3212,  -2410,  -1608,  -804,   0,
};
#endif

/* Internal sine generator (static inline for performance)
 * Input:  phase in [0, Q15_MAX]
 * Output: sine value in [-Q15_MAX, Q15_MAX]
 */
static inline q15_t picosynth_sine_impl(q15_t phase)
{
#if defined(PICOSYNTH_SINE_LUT_8BIT) && PICOSYNTH_SINE_LUT_8BIT
    int idx = (phase >> 8) & 0x7F;
    q15_t r = sine_lut8[idx] * 258;
#if PICOSYNTH_INTERPOLATE
    q15_t next = sine_lut8[idx + 1] * 258;
    r += (q15_t) (((next - r) * (phase & 0xFF)) >> 8);
#endif
    return r;
#elif defined(PICOSYNTH_SINE_LUT_16BIT) && PICOSYNTH_SINE_LUT_16BIT
    int idx = (phase >> 7) & 0xFF;
    q15_t r = sine_lut16[idx];
#if PICOSYNTH_INTERPOLATE
    q15_t next = sine_lut16[idx + 1];
    r += (q15_t) (((next - r) * (phase & 0x7F)) >> 7);
#endif
    return r;
#elif defined(PICOSYNTH_USE_SINF)
    float angle = (float) phase * (2.0f * (float) M_PI / (Q15_MAX + 1.0f));
    return (q15_t) (sinf(angle) * Q15_MAX);
#else
#error "No sine implementation selected"
#endif
}

#endif /* PICOSYNTH_MATH_H_ */
