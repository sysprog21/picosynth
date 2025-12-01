/* Unit tests for Q15 fixed-point arithmetic */

#include "picosynth.h"
#include "test.h"

/* Test q15_sat saturation */
static void test_q15_sat_positive(void)
{
    /* Values within range should pass through */
    TEST_ASSERT_EQ(q15_sat(0), 0, "q15_sat(0)");
    TEST_ASSERT_EQ(q15_sat(1000), 1000, "q15_sat(1000)");
    TEST_ASSERT_EQ(q15_sat(Q15_MAX), Q15_MAX, "q15_sat(Q15_MAX)");
    TEST_ASSERT_EQ(q15_sat(-1000), -1000, "q15_sat(-1000)");
    TEST_ASSERT_EQ(q15_sat(-32768), -32768, "q15_sat(-32768)");
}

static void test_q15_sat_overflow(void)
{
    /* Values above Q15_MAX should saturate to Q15_MAX */
    TEST_ASSERT_EQ(q15_sat(Q15_MAX + 1), Q15_MAX, "q15_sat overflow +1");
    TEST_ASSERT_EQ(q15_sat(50000), Q15_MAX, "q15_sat overflow 50000");
    TEST_ASSERT_EQ(q15_sat(0x7FFFFFFF), Q15_MAX, "q15_sat overflow max int32");
}

static void test_q15_sat_underflow(void)
{
    /* Values below -32768 should saturate to Q15_MIN (-32768) */
    TEST_ASSERT_EQ(q15_sat(-32769), (q15_t) -32768, "q15_sat underflow -1");
    TEST_ASSERT_EQ(q15_sat(-50000), (q15_t) -32768, "q15_sat underflow -50000");
    TEST_ASSERT_EQ(q15_sat(-0x7FFFFFFF), (q15_t) -32768,
                   "q15_sat underflow min");
}

/* Test Q15 multiplication */
static void test_q15_multiply(void)
{
    /* 0.5 * 0.5 = 0.25 */
    q15_t half = Q15_MAX / 2;
    int32_t result = ((int32_t) half * half) >> 15;
    /* Expected: ~0.25 * Q15_MAX = ~8192 */
    TEST_ASSERT_RANGE(result, 8000, 8400, "0.5 * 0.5 approx 0.25");

    /* 1.0 * 0.5 = 0.5 */
    result = ((int32_t) Q15_MAX * half) >> 15;
    TEST_ASSERT_RANGE(result, half - 100, half + 100, "1.0 * 0.5 approx 0.5");

    /* -0.5 * 0.5 = -0.25 */
    result = ((int32_t) (-half) * half) >> 15;
    TEST_ASSERT_RANGE(result, -8400, -8000, "-0.5 * 0.5 approx -0.25");
}

/* Test Q15 constants */
static void test_q15_constants(void)
{
    TEST_ASSERT_EQ(Q15_MAX, 0x7FFF, "Q15_MAX value");
    /* Q15_MIN is 0x8000, which as q15_t (int16_t) is -32768 */
    TEST_ASSERT_EQ((q15_t) Q15_MIN, (q15_t) -32768, "Q15_MIN value");
    TEST_ASSERT_EQ(PICOSYNTH_BLOCK_SIZE, 32, "default block size");
}

/* Test PICOSYNTH_MS macro */
static void test_picosynth_ms(void)
{
    /* 1000ms = SAMPLE_RATE samples */
    TEST_ASSERT_EQ(PICOSYNTH_MS(1000), SAMPLE_RATE, "1000ms = SAMPLE_RATE");

    /* 100ms = SAMPLE_RATE/10 samples */
    uint32_t expected = SAMPLE_RATE / 10;
    TEST_ASSERT_RANGE(PICOSYNTH_MS(100), expected - 1, expected + 1,
                      "100ms approx SAMPLE_RATE/10");

    /* 0ms = 0 samples */
    TEST_ASSERT_EQ(PICOSYNTH_MS(0), 0, "0ms = 0 samples");
}

void test_q15_all(void)
{
    TEST_RUN(test_q15_sat_positive);
    TEST_RUN(test_q15_sat_overflow);
    TEST_RUN(test_q15_sat_underflow);
    TEST_RUN(test_q15_multiply);
    TEST_RUN(test_q15_constants);
    TEST_RUN(test_picosynth_ms);
}
