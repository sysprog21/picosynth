/* Unit tests for waveform generators */
#include "picosynth.h"
#include "test.h"

/* Test sine wave properties */
static void test_wave_sine_range(void)
{
    /* Sine should output values in Q15 range [-32768, 32767] */
    /* Test a few key phases instead of iterating many times */
    q15_t phases[] = {0,           Q15_MAX / 8,     Q15_MAX / 4,
                      Q15_MAX / 2, Q15_MAX * 3 / 4, Q15_MAX - 1};
    for (int i = 0; i < 6; i++) {
        q15_t val = picosynth_wave_sine(phases[i]);
        /* q15_t is always in range by definition, verify no extreme saturation
         */
        TEST_ASSERT(val >= -32767 && val <= 32767, "sine output in Q15 range");
    }
}

static void test_wave_sine_zero_crossing(void)
{
    /* Sine at phase 0 should be near 0 */
    q15_t val = picosynth_wave_sine(0);
    TEST_ASSERT_RANGE(val, -100, 100, "sine(0) near zero");

    /* Sine at phase Q15_MAX/2 should be near 0 */
    val = picosynth_wave_sine(Q15_MAX / 2);
    TEST_ASSERT_RANGE(val, -100, 100, "sine(pi) near zero");
}

static void test_wave_sine_extremes(void)
{
    /* Sine at phase Q15_MAX/4 should be near maximum */
    q15_t val = picosynth_wave_sine(Q15_MAX / 4);
    TEST_ASSERT(val > 29000, "sine(pi/2) near maximum");

    /* Sine at phase 3*Q15_MAX/4 should be near minimum */
    val = picosynth_wave_sine(Q15_MAX * 3 / 4);
    TEST_ASSERT(val < -29000, "sine(3pi/2) near minimum");
}

/* Test square wave */
static void test_wave_square(void)
{
    /* First half should be positive (Q15_MAX) */
    q15_t val = picosynth_wave_square(Q15_MAX / 4);
    TEST_ASSERT_EQ(val, Q15_MAX, "square first half is Q15_MAX");

    /* Second half should be Q15_MIN (-32768) per implementation */
    val = picosynth_wave_square(Q15_MAX * 3 / 4);
    TEST_ASSERT_EQ(val, (q15_t) Q15_MIN, "square second half is Q15_MIN");
}

/* Test sawtooth wave (rising saw: starts at -Q15_MAX, rises to Q15_MAX) */
static void test_wave_saw(void)
{
    /* Sawtooth at phase 0: 0 * 2 - Q15_MAX = -Q15_MAX */
    q15_t val = picosynth_wave_saw(0);
    TEST_ASSERT_EQ(val, -Q15_MAX, "saw(0) is -Q15_MAX (rising saw)");

    /* Sawtooth at Q15_MAX: Q15_MAX * 2 - Q15_MAX = Q15_MAX */
    val = picosynth_wave_saw(Q15_MAX);
    TEST_ASSERT_EQ(val, Q15_MAX, "saw(max) is Q15_MAX");

    /* Sawtooth at Q15_MAX/2: should be near 0 */
    val = picosynth_wave_saw(Q15_MAX / 2);
    TEST_ASSERT_RANGE(val, -100, 100, "saw(mid) near zero");

    /* Sawtooth should be monotonically increasing (spot check) */
    q15_t prev = picosynth_wave_saw(0);
    q15_t test_phases[] = {1000, 5000, 10000, 20000, 30000};
    for (int i = 0; i < 5; i++) {
        val = picosynth_wave_saw(test_phases[i]);
        TEST_ASSERT(val >= prev, "saw increasing");
        prev = val;
    }
}

/* Test falling ramp wave (starts at Q15_MAX, falls to -Q15_MAX) */
static void test_wave_falling(void)
{
    /* Falling at phase 0: Q15_MAX - 0 * 2 = Q15_MAX */
    q15_t val = picosynth_wave_falling(0);
    TEST_ASSERT_EQ(val, Q15_MAX, "falling(0) is Q15_MAX");

    /* Falling at Q15_MAX: Q15_MAX - Q15_MAX * 2 = -Q15_MAX */
    val = picosynth_wave_falling(Q15_MAX);
    TEST_ASSERT_EQ(val, -Q15_MAX, "falling(max) is -Q15_MAX");

    /* Falling should be monotonically decreasing (spot check) */
    q15_t prev = picosynth_wave_falling(0);
    q15_t test_phases[] = {1000, 5000, 10000, 20000, 30000};
    for (int i = 0; i < 5; i++) {
        val = picosynth_wave_falling(test_phases[i]);
        TEST_ASSERT(val <= prev, "falling decreasing");
        prev = val;
    }
}

/* Test triangle wave */
static void test_wave_triangle(void)
{
    /* Triangle at 0 should be -Q15_MAX */
    q15_t val = picosynth_wave_triangle(0);
    TEST_ASSERT_EQ(val, -Q15_MAX, "triangle(0) is -Q15_MAX");

    /* Triangle at Q15_MAX/2: due to formula rounding, may not hit exact Q15_MAX
     */
    /* Formula: r = phase << 1; if r > Q15_MAX: r = Q15_MAX - (r - Q15_MAX);
     * return q15_sat(r * 2 - Q15_MAX) */
    /* At Q15_MAX/2 (16383): r = 32766; 32766 <= 32767; result = 32766 * 2 -
     * 32767 = 32765 */
    val = picosynth_wave_triangle(Q15_MAX / 2);
    TEST_ASSERT_RANGE(val, 32760, Q15_MAX, "triangle(mid) near Q15_MAX");
}

/* Test noise generator */
static void test_wave_noise(void)
{
    /* Noise should produce varying values */
    q15_t first = picosynth_wave_noise(0);
    q15_t second = picosynth_wave_noise(0);
    q15_t third = picosynth_wave_noise(0);

    /* At least some values should differ */
    int varied = (first != second) || (second != third) || (first != third);
    TEST_ASSERT(varied, "noise produces varied output");
}

/* Test exponential wave */
static void test_wave_exp(void)
{
    /* Exp formula: p = Q15_MAX - phase; p = (p*p)>>15; p = (p*p)>>15; return p
     */
    /* At phase 0: p = 32767; p = 32766 (after first square); p = 32764 (after
     * second) */
    q15_t val = picosynth_wave_exp(0);
    TEST_ASSERT_RANGE(val, 32760, Q15_MAX, "exp(0) near Q15_MAX");

    /* Exp at Q15_MAX should be near 0 */
    val = picosynth_wave_exp(Q15_MAX);
    TEST_ASSERT_RANGE(val, 0, 100, "exp(max) near zero");

    /* Exp should be monotonically decreasing (spot check) */
    q15_t prev = picosynth_wave_exp(0);
    q15_t test_phases[] = {1000, 5000, 10000, 20000, 30000};
    for (int i = 0; i < 5; i++) {
        val = picosynth_wave_exp(test_phases[i]);
        TEST_ASSERT(val <= prev, "exp decreasing");
        prev = val;
    }
}

void test_waveform_all(void)
{
    TEST_RUN(test_wave_sine_range);
    TEST_RUN(test_wave_sine_zero_crossing);
    TEST_RUN(test_wave_sine_extremes);
    TEST_RUN(test_wave_square);
    TEST_RUN(test_wave_saw);
    TEST_RUN(test_wave_falling);
    TEST_RUN(test_wave_triangle);
    TEST_RUN(test_wave_noise);
    TEST_RUN(test_wave_exp);
}
