/* Test suite driver */

#include <stdio.h>
#include "test.h"

/* Define test counters (declared extern in test.h) */
int test_pass_count;
int test_fail_count;

/* Test suite declarations */
extern void test_q15_all(void);
extern void test_waveform_all(void);
extern void test_envelope_all(void);
extern void test_synth_all(void);

int main(void)
{
    TEST_INIT();

    printf("=== PicoSynth Unit Tests ===\n\n");

    printf("--- Q15 Fixed-Point Tests ---\n");
    test_q15_all();

    printf("\n--- Waveform Generator Tests ---\n");
    test_waveform_all();

    printf("\n--- Envelope Tests ---\n");
    test_envelope_all();

    printf("\n--- Synthesizer Core Tests ---\n");
    test_synth_all();

    TEST_SUMMARY();

    return TEST_RESULT();
}
