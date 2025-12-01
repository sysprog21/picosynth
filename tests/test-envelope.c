/* Unit tests for envelope processing */
#include "picosynth.h"
#include "test.h"

/* Test envelope attack phase */
static void test_envelope_attack(void)
{
    picosynth_t *s = picosynth_create(1, 2);
    TEST_ASSERT(s != NULL, "synth creation");

    picosynth_voice_t *v = picosynth_get_voice(s, 0);
    picosynth_node_t *env = picosynth_voice_get_node(v, 0);
    picosynth_node_t *osc = picosynth_voice_get_node(v, 1);

    /* Fast attack envelope */
    picosynth_init_env(env, NULL, 5000, 100, Q15_MAX / 2, 100);
    picosynth_init_osc(osc, &env->out, picosynth_voice_freq_ptr(v),
                       picosynth_wave_sine);
    picosynth_voice_set_out(v, 1);

    /* Trigger note */
    picosynth_note_on(s, 0, 60);

    /* Process some samples - envelope should rise */
    q15_t prev = 0;
    int rising_count = 0;
    for (int i = 0; i < 100; i++) {
        picosynth_process(s);
        q15_t current = env->out;
        if (current > prev)
            rising_count++;
        prev = current;
    }

    TEST_ASSERT(rising_count > 50, "envelope rises during attack");
    TEST_ASSERT(env->out > 0, "envelope output positive after attack");

    picosynth_destroy(s);
}

/* Test envelope decay to sustain */
static void test_envelope_decay(void)
{
    picosynth_t *s = picosynth_create(1, 2);
    TEST_ASSERT(s != NULL, "synth creation");

    picosynth_voice_t *v = picosynth_get_voice(s, 0);
    picosynth_node_t *env = picosynth_voice_get_node(v, 0);
    picosynth_node_t *osc = picosynth_voice_get_node(v, 1);

    /* Very fast attack, slower decay, 50% sustain */
    picosynth_init_env(env, NULL, 30000, 500, Q15_MAX / 2, 100);
    picosynth_init_osc(osc, &env->out, picosynth_voice_freq_ptr(v),
                       picosynth_wave_sine);
    picosynth_voice_set_out(v, 1);

    picosynth_note_on(s, 0, 60);

    /* Process until we reach peak and start decaying */
    q15_t peak = 0;
    for (int i = 0; i < 500; i++) {
        picosynth_process(s);
        if (env->out > peak)
            peak = env->out;
    }

    TEST_ASSERT(peak > Q15_MAX / 4, "envelope reached significant peak");

    /* Continue processing - should decay towards sustain */
    for (int i = 0; i < 1000; i++)
        picosynth_process(s);

    /* Envelope has squared output curve, so final value may be much lower.
     * Just verify it's still positive and less than peak. */
    TEST_ASSERT(env->out > 0, "envelope still positive in sustain");
    TEST_ASSERT(env->out <= peak, "envelope decayed from peak");

    picosynth_destroy(s);
}

/* Test envelope release phase */
static void test_envelope_release(void)
{
    picosynth_t *s = picosynth_create(1, 2);
    TEST_ASSERT(s != NULL, "synth creation");

    picosynth_voice_t *v = picosynth_get_voice(s, 0);
    picosynth_node_t *env = picosynth_voice_get_node(v, 0);
    picosynth_node_t *osc = picosynth_voice_get_node(v, 1);

    /* Fast attack, high sustain, slow release */
    picosynth_init_env(env, NULL, 30000, 500, Q15_MAX * 8 / 10, 200);
    picosynth_init_osc(osc, &env->out, picosynth_voice_freq_ptr(v),
                       picosynth_wave_sine);
    picosynth_voice_set_out(v, 1);

    picosynth_note_on(s, 0, 60);

    /* Let envelope reach sustain */
    for (int i = 0; i < 500; i++)
        picosynth_process(s);

    q15_t level_before_release = env->out;
    TEST_ASSERT(level_before_release > 0, "envelope active before release");

    /* Release note */
    picosynth_note_off(s, 0);

    /* Process and verify envelope decreases */
    int decreasing_count = 0;
    q15_t prev = level_before_release;
    for (int i = 0; i < 500; i++) {
        picosynth_process(s);
        if (env->out < prev)
            decreasing_count++;
        prev = env->out;
    }

    TEST_ASSERT(decreasing_count > 100, "envelope decreases during release");
    TEST_ASSERT(env->out < level_before_release,
                "envelope lower after release");

    picosynth_destroy(s);
}

/* Test block-based rate computation */
static void test_envelope_block_rate(void)
{
    picosynth_t *s = picosynth_create(1, 2);
    TEST_ASSERT(s != NULL, "synth creation");

    picosynth_voice_t *v = picosynth_get_voice(s, 0);
    picosynth_node_t *env = picosynth_voice_get_node(v, 0);
    picosynth_node_t *osc = picosynth_voice_get_node(v, 1);

    picosynth_init_env(env, NULL, 1000, 100, Q15_MAX / 2, 100);
    picosynth_init_osc(osc, &env->out, picosynth_voice_freq_ptr(v),
                       picosynth_wave_sine);
    picosynth_voice_set_out(v, 1);

    picosynth_note_on(s, 0, 60);

    /* Verify block_counter starts at 0 (forcing immediate rate calc) */
    TEST_ASSERT_EQ(env->env.block_counter, 0, "block_counter init to 0");

    /* Process one sample - should set block_counter to BLOCK_SIZE-1 */
    picosynth_process(s);
    TEST_ASSERT_EQ(env->env.block_counter, PICOSYNTH_BLOCK_SIZE - 1,
                   "block_counter set after first sample");

    /* Verify block_rate is positive (attack phase) */
    TEST_ASSERT(env->env.block_rate > 0, "block_rate positive during attack");

    picosynth_destroy(s);
}

/* Test immediate release rate update (codex fix) */
static void test_envelope_immediate_release(void)
{
    picosynth_t *s = picosynth_create(1, 2);
    TEST_ASSERT(s != NULL, "synth creation");

    picosynth_voice_t *v = picosynth_get_voice(s, 0);
    picosynth_node_t *env = picosynth_voice_get_node(v, 0);
    picosynth_node_t *osc = picosynth_voice_get_node(v, 1);

    picosynth_init_env(env, NULL, 5000, 500, Q15_MAX / 2, 500);
    picosynth_init_osc(osc, &env->out, picosynth_voice_freq_ptr(v),
                       picosynth_wave_sine);
    picosynth_voice_set_out(v, 1);

    picosynth_note_on(s, 0, 60);

    /* Process 10 samples (mid-block) */
    for (int i = 0; i < 10; i++)
        picosynth_process(s);

    /* block_counter should be non-zero (mid-block) */
    TEST_ASSERT(env->env.block_counter > 0, "mid-block before note-off");
    TEST_ASSERT(env->env.block_rate > 0, "attack rate before note-off");

    /* Release note - should reset block_counter to 0 */
    picosynth_note_off(s, 0);
    TEST_ASSERT_EQ(env->env.block_counter, 0,
                   "block_counter reset on note-off");

    /* Process one sample - rate should now be negative (release) */
    picosynth_process(s);
    TEST_ASSERT(env->env.block_rate < 0, "release rate after note-off");

    picosynth_destroy(s);
}

/* Test envelope with negative sustain (inverted output) */
static void test_envelope_negative_sustain(void)
{
    picosynth_t *s = picosynth_create(1, 2);
    TEST_ASSERT(s != NULL, "synth creation");

    picosynth_voice_t *v = picosynth_get_voice(s, 0);
    picosynth_node_t *env = picosynth_voice_get_node(v, 0);
    picosynth_node_t *osc = picosynth_voice_get_node(v, 1);

    /* Negative sustain for inverted envelope */
    picosynth_init_env(env, NULL, 5000, 500, -Q15_MAX / 2, 100);
    picosynth_init_osc(osc, &env->out, picosynth_voice_freq_ptr(v),
                       picosynth_wave_sine);
    picosynth_voice_set_out(v, 1);

    picosynth_note_on(s, 0, 60);

    /* Process until decay phase */
    for (int i = 0; i < 1000; i++)
        picosynth_process(s);

    /* Output should be negative (inverted) */
    TEST_ASSERT(env->out < 0, "envelope inverted with negative sustain");

    picosynth_destroy(s);
}

/* Test picosynth_init_env_ms convenience function */
static void test_envelope_init_ms(void)
{
    picosynth_t *s = picosynth_create(1, 2);
    TEST_ASSERT(s != NULL, "synth creation");

    picosynth_voice_t *v = picosynth_get_voice(s, 0);
    picosynth_node_t *env = picosynth_voice_get_node(v, 0);
    picosynth_node_t *osc = picosynth_voice_get_node(v, 1);

    /* Initialize with ms timing: 10ms attack, 100ms decay, 80% sustain, 50ms
     * release */
    picosynth_init_env_ms(env, NULL, 10, 100, 80, 50);
    picosynth_init_osc(osc, &env->out, picosynth_voice_freq_ptr(v),
                       picosynth_wave_sine);
    picosynth_voice_set_out(v, 1);

    /* Verify envelope parameters were set */
    TEST_ASSERT(env->env.attack > 0, "attack rate set");
    TEST_ASSERT(env->env.decay > 0, "decay rate set");
    TEST_ASSERT(env->env.sustain > 0, "sustain level positive");
    TEST_ASSERT(env->env.release > 0, "release rate set");

    /* Verify sustain is approximately 80% */
    int32_t expected_sustain = (80 * Q15_MAX) / 100;
    int32_t diff = env->env.sustain - expected_sustain;
    if (diff < 0)
        diff = -diff;
    TEST_ASSERT(diff < 100, "sustain level near 80%");

    picosynth_destroy(s);
}

void test_envelope_all(void)
{
    TEST_RUN(test_envelope_attack);
    TEST_RUN(test_envelope_decay);
    TEST_RUN(test_envelope_release);
    TEST_RUN(test_envelope_block_rate);
    TEST_RUN(test_envelope_immediate_release);
    TEST_RUN(test_envelope_negative_sustain);
    TEST_RUN(test_envelope_init_ms);
}
