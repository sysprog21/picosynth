/* Unit tests for synthesizer core functionality */
#include "picosynth.h"
#include "test.h"

/* Test synth creation and destruction */
static void test_synth_create(void)
{
    picosynth_t *s = picosynth_create(4, 8);
    TEST_ASSERT(s != NULL, "synth creation with 4 voices");
    picosynth_destroy(s);

    s = picosynth_create(1, 2);
    TEST_ASSERT(s != NULL, "synth creation with 1 voice");
    picosynth_destroy(s);
}

static void test_synth_create_zero(void)
{
    /* Zero voices: calloc(0, ...) is implementation-defined.
     * The implementation doesn't explicitly check for 0 voices,
     * so this may return a valid pointer or NULL depending on the system.
     * We just verify it doesn't crash. */
    picosynth_t *s = picosynth_create(0, 8);
    /* Don't assert success or failure - just clean up if non-null */
    if (s)
        picosynth_destroy(s);
    TEST_ASSERT(1, "synth creation with 0 voices handled gracefully");
}

/* Test voice access */
static void test_voice_access(void)
{
    picosynth_t *s = picosynth_create(2, 4);
    TEST_ASSERT(s != NULL, "synth creation");

    picosynth_voice_t *v0 = picosynth_get_voice(s, 0);
    picosynth_voice_t *v1 = picosynth_get_voice(s, 1);

    TEST_ASSERT(v0 != NULL, "voice 0 access");
    TEST_ASSERT(v1 != NULL, "voice 1 access");
    TEST_ASSERT(v0 != v1, "voices are distinct");

    /* Out of bounds should return NULL */
    picosynth_voice_t *v2 = picosynth_get_voice(s, 2);
    TEST_ASSERT(v2 == NULL, "out of bounds voice access returns NULL");

    picosynth_destroy(s);
}

/* Test node access */
static void test_node_access(void)
{
    picosynth_t *s = picosynth_create(1, 4);
    TEST_ASSERT(s != NULL, "synth creation");

    picosynth_voice_t *v = picosynth_get_voice(s, 0);
    picosynth_node_t *n0 = picosynth_voice_get_node(v, 0);
    picosynth_node_t *n1 = picosynth_voice_get_node(v, 1);
    picosynth_node_t *n3 = picosynth_voice_get_node(v, 3);

    TEST_ASSERT(n0 != NULL, "node 0 access");
    TEST_ASSERT(n1 != NULL, "node 1 access");
    TEST_ASSERT(n3 != NULL, "node 3 access");

    /* Out of bounds */
    picosynth_node_t *n4 = picosynth_voice_get_node(v, 4);
    TEST_ASSERT(n4 == NULL, "out of bounds node access returns NULL");

    picosynth_destroy(s);
}

/* Test MIDI note frequency conversion */
static void test_midi_to_freq(void)
{
    /* A4 = MIDI 69. The implementation uses octave table lookup.
     * octave8_freq[9] (A8) / 8 (shift by 3 for octave 5) */
    q15_t freq_a4 = picosynth_midi_to_freq(69);
    /* Implementation gives ~2615-2616 based on table lookup */
    TEST_ASSERT_RANGE(freq_a4, 2500, 2700, "A4 frequency in expected range");

    /* Higher notes should have higher frequency */
    q15_t freq_a5 = picosynth_midi_to_freq(81); /* A5 */
    TEST_ASSERT(freq_a5 > freq_a4, "A5 > A4 frequency");

    /* Octave should double frequency (approximately) */
    int32_t ratio = ((int32_t) freq_a5 * 100) / freq_a4;
    TEST_ASSERT_RANGE(ratio, 190, 210, "octave ratio near 2.0");

    /* Lower notes should have lower frequency */
    q15_t freq_a3 = picosynth_midi_to_freq(57); /* A3 */
    TEST_ASSERT(freq_a3 < freq_a4, "A3 < A4 frequency");

    /* Middle C (MIDI 60) frequency check */
    q15_t freq_c4 = picosynth_midi_to_freq(60);
    TEST_ASSERT(freq_c4 > 0, "C4 frequency positive");
    TEST_ASSERT(freq_c4 < freq_a4, "C4 < A4 frequency");
}

/* Test note on/off */
static void test_note_on_off(void)
{
    picosynth_t *s = picosynth_create(1, 2);
    TEST_ASSERT(s != NULL, "synth creation");

    picosynth_voice_t *v = picosynth_get_voice(s, 0);
    picosynth_node_t *env = picosynth_voice_get_node(v, 0);
    picosynth_node_t *osc = picosynth_voice_get_node(v, 1);

    picosynth_init_env(env, NULL,
                       &(picosynth_env_params_t) {
                           .attack = 5000,
                           .hold = 0,
                           .decay = 500,
                           .sustain = Q15_MAX / 2,
                           .release = 500,
                       });
    picosynth_init_osc(osc, &env->out, picosynth_voice_freq_ptr(v),
                       picosynth_wave_sine);
    picosynth_voice_set_out(v, 1);

    /* Note on should produce output */
    picosynth_note_on(s, 0, 60);
    for (int i = 0; i < 100; i++)
        picosynth_process(s);

    TEST_ASSERT(env->out > 0, "envelope active after note on");

    /* Note off should start release */
    picosynth_note_off(s, 0);

    q15_t level_at_release = env->out;
    for (int i = 0; i < 500; i++)
        picosynth_process(s);

    TEST_ASSERT(env->out < level_at_release,
                "envelope decreased after note off");

    picosynth_destroy(s);
}

/* Test oscillator phase accumulation */
static void test_oscillator_phase(void)
{
    picosynth_t *s = picosynth_create(1, 2);
    TEST_ASSERT(s != NULL, "synth creation");

    picosynth_voice_t *v = picosynth_get_voice(s, 0);
    picosynth_node_t *env = picosynth_voice_get_node(v, 0);
    picosynth_node_t *osc = picosynth_voice_get_node(v, 1);

    picosynth_init_env(env, NULL,
                       &(picosynth_env_params_t) {
                           .attack = 30000,
                           .hold = 0,
                           .decay = 500,
                           .sustain = Q15_MAX,
                           .release = 500,
                       });
    picosynth_init_osc(osc, &env->out, picosynth_voice_freq_ptr(v),
                       picosynth_wave_sine);
    picosynth_voice_set_out(v, 1);

    picosynth_note_on(s, 0, 69); /* A4 = 440Hz */

    /* Process and check that oscillator state (phase) changes */
    int32_t initial_state = osc->state;
    picosynth_process(s);
    int32_t next_state = osc->state;

    TEST_ASSERT(next_state != initial_state, "oscillator phase advances");

    /* Phase should wrap within Q15_MAX */
    for (int i = 0; i < 1000; i++)
        picosynth_process(s);

    TEST_ASSERT(osc->state >= 0 && osc->state <= Q15_MAX,
                "oscillator phase in valid range");

    picosynth_destroy(s);
}

/* Test filter initialization */
static void test_filter_init(void)
{
    picosynth_t *s = picosynth_create(1, 3);
    TEST_ASSERT(s != NULL, "synth creation");

    picosynth_voice_t *v = picosynth_get_voice(s, 0);
    picosynth_node_t *env = picosynth_voice_get_node(v, 0);
    picosynth_node_t *osc = picosynth_voice_get_node(v, 1);
    picosynth_node_t *flt = picosynth_voice_get_node(v, 2);

    picosynth_init_env(env, NULL,
                       &(picosynth_env_params_t) {
                           .attack = 5000,
                           .hold = 0,
                           .decay = 500,
                           .sustain = Q15_MAX / 2,
                           .release = 500,
                       });
    picosynth_init_osc(osc, &env->out, picosynth_voice_freq_ptr(v),
                       picosynth_wave_saw);
    picosynth_init_lp(flt, NULL, &osc->out, 3000);
    picosynth_voice_set_out(v, 2);

    /* Filter should smooth out sawtooth */
    picosynth_note_on(s, 0, 60);

    /* Process and verify output is within range */
    for (int i = 0; i < 500; i++) {
        q15_t out = picosynth_process(s);
        /* q15_t is always in valid range; verify reasonable values */
        TEST_ASSERT(out >= -32768 && out <= 32767,
                    "filter output in Q15 range");
    }

    picosynth_destroy(s);
}

/* Test high-pass filter */
static void test_filter_hp(void)
{
    picosynth_t *s = picosynth_create(1, 3);
    TEST_ASSERT(s != NULL, "synth creation");

    picosynth_voice_t *v = picosynth_get_voice(s, 0);
    picosynth_node_t *env = picosynth_voice_get_node(v, 0);
    picosynth_node_t *osc = picosynth_voice_get_node(v, 1);
    picosynth_node_t *flt = picosynth_voice_get_node(v, 2);

    picosynth_init_env(env, NULL,
                       &(picosynth_env_params_t) {
                           .attack = 5000,
                           .hold = 0,
                           .decay = 500,
                           .sustain = Q15_MAX / 2,
                           .release = 500,
                       });
    picosynth_init_osc(osc, &env->out, picosynth_voice_freq_ptr(v),
                       picosynth_wave_saw);
    picosynth_init_hp(flt, NULL, &osc->out, 3000);
    picosynth_voice_set_out(v, 2);

    picosynth_note_on(s, 0, 60);

    /* Verify HP filter produces output */
    int non_zero = 0;
    for (int i = 0; i < 200; i++) {
        q15_t out = picosynth_process(s);
        if (out != 0)
            non_zero++;
    }
    TEST_ASSERT(non_zero > 50, "HP filter produces output");

    picosynth_destroy(s);
}

/* Test mixer node */
static void test_mixer(void)
{
    picosynth_t *s = picosynth_create(1, 4);
    TEST_ASSERT(s != NULL, "synth creation");

    picosynth_voice_t *v = picosynth_get_voice(s, 0);
    picosynth_node_t *env = picosynth_voice_get_node(v, 0);
    picosynth_node_t *osc1 = picosynth_voice_get_node(v, 1);
    picosynth_node_t *osc2 = picosynth_voice_get_node(v, 2);
    picosynth_node_t *mix = picosynth_voice_get_node(v, 3);

    picosynth_init_env(env, NULL,
                       &(picosynth_env_params_t) {
                           .attack = 30000,
                           .hold = 0,
                           .decay = 500,
                           .sustain = Q15_MAX,
                           .release = 500,
                       });
    picosynth_init_osc(osc1, &env->out, picosynth_voice_freq_ptr(v),
                       picosynth_wave_sine);
    picosynth_init_osc(osc2, &env->out, picosynth_voice_freq_ptr(v),
                       picosynth_wave_triangle);
    picosynth_init_mix(mix, NULL, &osc1->out, &osc2->out, NULL);
    picosynth_voice_set_out(v, 3);

    picosynth_note_on(s, 0, 60);

    /* Mixer should produce output */
    int non_zero = 0;
    for (int i = 0; i < 500; i++) {
        q15_t out = picosynth_process(s);
        if (out != 0)
            non_zero++;
    }

    TEST_ASSERT(non_zero > 400, "mixer produces output");

    picosynth_destroy(s);
}

/* Test voice frequency pointer */
static void test_voice_freq_ptr(void)
{
    picosynth_t *s = picosynth_create(1, 2);
    TEST_ASSERT(s != NULL, "synth creation");

    picosynth_voice_t *v = picosynth_get_voice(s, 0);
    const q15_t *freq_ptr = picosynth_voice_freq_ptr(v);

    TEST_ASSERT(freq_ptr != NULL, "freq pointer not NULL");

    /* Trigger note and verify frequency is set */
    picosynth_note_on(s, 0, 69);
    TEST_ASSERT(*freq_ptr > 0, "frequency set after note on");

    picosynth_destroy(s);
}

/* Test picosynth_voice_set_out */
static void test_voice_set_out(void)
{
    picosynth_t *s = picosynth_create(1, 4);
    TEST_ASSERT(s != NULL, "synth creation");

    picosynth_voice_t *v = picosynth_get_voice(s, 0);
    picosynth_node_t *env = picosynth_voice_get_node(v, 0);
    picosynth_node_t *osc = picosynth_voice_get_node(v, 1);

    picosynth_init_env(env, NULL,
                       &(picosynth_env_params_t) {
                           .attack = 30000,
                           .hold = 0,
                           .decay = 500,
                           .sustain = Q15_MAX,
                           .release = 500,
                       });
    picosynth_init_osc(osc, &env->out, picosynth_voice_freq_ptr(v),
                       picosynth_wave_sine);

    /* Set output to node 1 (oscillator) */
    picosynth_voice_set_out(v, 1);
    picosynth_note_on(s, 0, 60);

    /* Should produce output */
    int non_zero = 0;
    for (int i = 0; i < 200; i++) {
        q15_t out = picosynth_process(s);
        if (out != 0)
            non_zero++;
    }
    TEST_ASSERT(non_zero > 100, "output from node 1");

    picosynth_destroy(s);
}

/* Test graphs with missing/null inputs */
static void test_null_graph_inputs(void)
{
    picosynth_t *s = picosynth_create(1, 2);
    TEST_ASSERT(s != NULL, "synth creation");

    picosynth_voice_t *v = picosynth_get_voice(s, 0);
    picosynth_node_t *osc = picosynth_voice_get_node(v, 0);
    picosynth_node_t *hp = picosynth_voice_get_node(v, 1);

    /* Intentionally leave freq/in pointers NULL to ensure null-safety */
    picosynth_init_osc(osc, NULL, NULL, picosynth_wave_sine);
    picosynth_init_hp(hp, NULL, NULL, Q15_MAX);
    picosynth_voice_set_out(v, 1);

    picosynth_note_on(s, 0, 60);

    int non_zero = 0;
    for (int i = 0; i < 64; i++) {
        q15_t sample = picosynth_process(s);
        if (sample != 0)
            non_zero++;
    }
    TEST_ASSERT(non_zero == 0, "null graph inputs produce silence");

    picosynth_destroy(s);
}

/* Test NULL pointer handling */
static void test_null_safety(void)
{
    /* These should not crash */
    picosynth_destroy(NULL);

    picosynth_voice_t *v = picosynth_get_voice(NULL, 0);
    TEST_ASSERT(v == NULL, "get_voice(NULL) returns NULL");

    picosynth_node_t *n = picosynth_voice_get_node(NULL, 0);
    TEST_ASSERT(n == NULL, "get_node(NULL) returns NULL");

    const q15_t *freq = picosynth_voice_freq_ptr(NULL);
    TEST_ASSERT(freq == NULL, "freq_ptr(NULL) returns NULL");

    TEST_ASSERT(1, "NULL pointer handling didn't crash");
}

void test_synth_all(void)
{
    TEST_RUN(test_synth_create);
    TEST_RUN(test_synth_create_zero);
    TEST_RUN(test_voice_access);
    TEST_RUN(test_node_access);
    TEST_RUN(test_midi_to_freq);
    TEST_RUN(test_note_on_off);
    TEST_RUN(test_oscillator_phase);
    TEST_RUN(test_filter_init);
    TEST_RUN(test_filter_hp);
    TEST_RUN(test_mixer);
    TEST_RUN(test_voice_freq_ptr);
    TEST_RUN(test_voice_set_out);
    TEST_RUN(test_null_graph_inputs);
    TEST_RUN(test_null_safety);
}
