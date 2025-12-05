/*
 * WebAssembly bindings for PicoSynth engine
 *
 * Exports functions callable from JavaScript via Emscripten.
 * Sample rate is set via -DSAMPLE_RATE in Makefile (default: 44100 Hz).
 */

#include <emscripten.h>
#include <stdlib.h>
#include <string.h>

#include "picosynth.h"

/* Global synth instance */
static picosynth_t *picosynth_instance = NULL;
static int16_t *picosynth_buffer = NULL;
static uint32_t picosynth_buffer_size = 0;

/* True partial frequency offsets for piano-like timbre */
static q15_t partial2_offset; /* 2nd partial: produces 2×f₁ */
static q15_t partial3_offset; /* 3rd partial: produces 3×f₁ */

/* Filter node pointers for dynamic frequency tracking */
static picosynth_node_t *g_flt_main;
static picosynth_node_t *g_flt_harm;
static picosynth_node_t *g_flt_noise;

/* Inharmonicity coefficient table (Q15 format).
 * B scales with frequency squared: B ≈ 7e-5 * (f/440)^2
 */
static q15_t get_inharmonicity_coeff(uint8_t note)
{
    static const q15_t B_table[12] = {
        1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 3, 3,
    };

    int octave = note / 12;
    int semitone = note % 12;

    int32_t B = B_table[semitone];
    for (int i = 0; i < octave - 4; i++)
        B <<= 2;
    for (int i = octave; i < 4; i++)
        B >>= 2;

    if (B < 1)
        B = 1;
    if (B > 65)
        B = 65;

    return (q15_t) B;
}

/* Calculate partial frequencies with inharmonicity stretching */
static void calc_partial_frequencies(uint8_t note, q15_t base_freq)
{
    q15_t B = get_inharmonicity_coeff(note);

    /* 2nd partial: freq = 2*f1*(1 + B*4/2)
     * Disable if 2*f1 >= Nyquist (Q15_MAX/2) to prevent aliasing.
     */
    if (2 * (int32_t) base_freq >= (Q15_MAX / 2)) {
        partial2_offset = 0; /* Would alias, disable this partial */
    } else {
        int32_t stretch2 = ((int32_t) B * 4 * base_freq) >> 15;
        partial2_offset = q15_sat(base_freq + stretch2);
    }

    /* 3rd partial: freq = 3*f1*(1 + B*9/2)
     * Disable if 3*f1 >= Nyquist (Q15_MAX/2) to prevent aliasing.
     */
    if (3 * (int32_t) base_freq >= (Q15_MAX / 2)) {
        partial3_offset = 0; /* Would alias, disable this partial */
    } else {
        int32_t stretch3 = ((int32_t) B * 14 * base_freq) >> 15;
        int32_t offset3 = 2 * (int32_t) base_freq + stretch3;
        partial3_offset = q15_sat(offset3);
    }
}

/* Frequency-tracked SVF filter coefficient */
static q15_t calc_svf_freq(uint8_t note)
{
    int32_t fc = 600 + 20 * ((int32_t) note - 48);
    if (fc < 500)
        fc = 500;
    if (fc > 1500)
        fc = 1500;

    return picosynth_svf_freq((uint16_t) fc);
}

/* Initialize voices with per-partial decay architecture */
static void init_piano_voices(void)
{
    if (!picosynth_instance)
        return;

    q15_t piano_q = Q15_MAX; /* Max damping, no resonance */

    /* Voice 0: FUNDAMENTAL (1st partial) - Slowest decay */
    picosynth_voice_t *v = picosynth_get_voice(picosynth_instance, 0);
    picosynth_node_t *v0_flt = picosynth_voice_get_node(v, 0);
    picosynth_node_t *v0_env = picosynth_voice_get_node(v, 1);
    picosynth_node_t *v0_osc = picosynth_voice_get_node(v, 2);

    picosynth_init_env(v0_env, NULL,
                       &(picosynth_env_params_t) {
                           .attack = 10000,
                           .hold = 0,
                           .decay = 60,
                           .sustain = (q15_t) (Q15_MAX * 15 / 100),
                           .release = 40,
                       });
    picosynth_init_osc(v0_osc, &v0_env->out, picosynth_voice_freq_ptr(v),
                       picosynth_wave_sine);
    picosynth_init_svf_lp(v0_flt, NULL, &v0_osc->out, picosynth_svf_freq(1200),
                          piano_q);
    g_flt_main = v0_flt;
    picosynth_voice_set_out(v, 0);

    /* Voice 1: 2nd-3rd PARTIALS - Medium decay */
    v = picosynth_get_voice(picosynth_instance, 1);
    picosynth_node_t *v1_flt = picosynth_voice_get_node(v, 0);
    picosynth_node_t *v1_env1 = picosynth_voice_get_node(v, 1);
    picosynth_node_t *v1_osc1 = picosynth_voice_get_node(v, 2);
    picosynth_node_t *v1_env2 = picosynth_voice_get_node(v, 3);
    picosynth_node_t *v1_osc2 = picosynth_voice_get_node(v, 4);
    picosynth_node_t *v1_mix = picosynth_voice_get_node(v, 5);

    picosynth_init_env(v1_env1, NULL,
                       &(picosynth_env_params_t) {
                           .attack = 8000,
                           .hold = 0,
                           .decay = 150,
                           .sustain = (q15_t) (Q15_MAX * 8 / 100),
                           .release = 50,
                       });
    picosynth_init_osc(v1_osc1, &v1_env1->out, picosynth_voice_freq_ptr(v),
                       picosynth_wave_sine);
    v1_osc1->osc.detune = &partial2_offset;

    picosynth_init_env(v1_env2, NULL,
                       &(picosynth_env_params_t) {
                           .attack = 7000,
                           .hold = 0,
                           .decay = 300,
                           .sustain = (q15_t) (Q15_MAX * 4 / 100),
                           .release = 40,
                       });
    picosynth_init_osc(v1_osc2, &v1_env2->out, picosynth_voice_freq_ptr(v),
                       picosynth_wave_sine);
    v1_osc2->osc.detune = &partial3_offset;

    picosynth_init_mix(v1_mix, NULL, &v1_osc1->out, &v1_osc2->out, NULL);
    picosynth_init_svf_lp(v1_flt, NULL, &v1_mix->out, picosynth_svf_freq(1200),
                          piano_q);
    g_flt_harm = v1_flt;
    picosynth_voice_set_out(v, 0);

    /* Voice 2: UPPER PARTIALS - Fast decay */
    v = picosynth_get_voice(picosynth_instance, 2);
    picosynth_node_t *v2_flt = picosynth_voice_get_node(v, 0);
    picosynth_node_t *v2_env = picosynth_voice_get_node(v, 1);
    picosynth_node_t *v2_osc = picosynth_voice_get_node(v, 2);

    picosynth_init_env(v2_env, NULL,
                       &(picosynth_env_params_t) {
                           .attack = 5000,
                           .hold = 0,
                           .decay = 800,
                           .sustain = (q15_t) (Q15_MAX * 1 / 100),
                           .release = 20,
                       });
    picosynth_init_osc(v2_osc, &v2_env->out, picosynth_voice_freq_ptr(v),
                       picosynth_wave_sine);
    picosynth_init_svf_lp(v2_flt, NULL, &v2_osc->out, picosynth_svf_freq(1500),
                          piano_q);
    picosynth_voice_set_out(v, 0);

    /* Voice 3: HAMMER NOISE - Very fast decay */
    v = picosynth_get_voice(picosynth_instance, 3);
    picosynth_node_t *v3_lp = picosynth_voice_get_node(v, 0);
    picosynth_node_t *v3_env = picosynth_voice_get_node(v, 1);
    picosynth_node_t *v3_noise = picosynth_voice_get_node(v, 2);
    picosynth_node_t *v3_hp = picosynth_voice_get_node(v, 3);

    picosynth_init_env(v3_env, NULL,
                       &(picosynth_env_params_t) {
                           .attack = 8000,
                           .hold = 0,
                           .decay = 6000,
                           .sustain = 0,
                           .release = 50,
                       });
    picosynth_init_osc(v3_noise, &v3_env->out, picosynth_voice_freq_ptr(v),
                       picosynth_wave_noise);
    picosynth_init_svf_hp(v3_hp, NULL, &v3_noise->out, picosynth_svf_freq(200),
                          piano_q);
    picosynth_init_svf_lp(v3_lp, NULL, &v3_hp->out, picosynth_svf_freq(800),
                          piano_q);
    g_flt_noise = v3_lp;
    picosynth_voice_set_out(v, 0);
}

/* Initialize synth engine */
EMSCRIPTEN_KEEPALIVE
int picosynth_wasm_init(void)
{
    if (picosynth_instance)
        picosynth_destroy(picosynth_instance);

    picosynth_instance = picosynth_create(4, 8);
    if (!picosynth_instance)
        return 0;

    init_piano_voices();
    return 1;
}

/* Clean up */
EMSCRIPTEN_KEEPALIVE
void picosynth_wasm_cleanup(void)
{
    if (picosynth_instance) {
        picosynth_destroy(picosynth_instance);
        picosynth_instance = NULL;
    }
    if (picosynth_buffer) {
        free(picosynth_buffer);
        picosynth_buffer = NULL;
        picosynth_buffer_size = 0;
    }
}

/* Trigger note on all 4 voices */
EMSCRIPTEN_KEEPALIVE
void picosynth_wasm_note_on(uint8_t note)
{
    if (!picosynth_instance)
        return;

    /* Trigger all 4 voices for per-partial decay */
    picosynth_note_on(picosynth_instance, 0, note);
    picosynth_note_on(picosynth_instance, 1, note);
    picosynth_note_on(picosynth_instance, 2, note);
    picosynth_note_on(picosynth_instance, 3, note);

    /* Calculate true partial frequencies with inharmonicity */
    q15_t base_freq =
        *picosynth_voice_freq_ptr(picosynth_get_voice(picosynth_instance, 0));
    calc_partial_frequencies(note, base_freq);

    /* Frequency-tracked filters */
    q15_t svf_f = calc_svf_freq(note);
    picosynth_svf_set_freq(g_flt_main, svf_f);

    int32_t fc_harm = 700 + 15 * ((int32_t) note - 48);
    if (fc_harm < 500)
        fc_harm = 500;
    if (fc_harm > 1400)
        fc_harm = 1400;
    picosynth_svf_set_freq(g_flt_harm, picosynth_svf_freq((uint16_t) fc_harm));

    int32_t fc_noise = 500 + 10 * ((int32_t) note - 48);
    if (fc_noise < 400)
        fc_noise = 400;
    if (fc_noise > 1000)
        fc_noise = 1000;
    picosynth_svf_set_freq(g_flt_noise,
                           picosynth_svf_freq((uint16_t) fc_noise));
}

/* Release note on all 4 voices */
EMSCRIPTEN_KEEPALIVE
void picosynth_wasm_note_off(void)
{
    if (!picosynth_instance)
        return;

    picosynth_note_off(picosynth_instance, 0);
    picosynth_note_off(picosynth_instance, 1);
    picosynth_note_off(picosynth_instance, 2);
    picosynth_note_off(picosynth_instance, 3);
}

/* Maximum buffer size to prevent overflow (60 seconds at 44100 Hz) */
#define MAX_BUFFER_SAMPLES (60UL * 44100UL)

/* Generate samples into internal buffer, return pointer */
EMSCRIPTEN_KEEPALIVE
int16_t *picosynth_wasm_render(uint32_t num_samples)
{
    if (!picosynth_instance)
        return NULL;

    if (num_samples == 0 || num_samples > MAX_BUFFER_SAMPLES)
        return NULL;

    if (num_samples > picosynth_buffer_size) {
        free(picosynth_buffer);
        picosynth_buffer = malloc(num_samples * sizeof(int16_t));
        if (!picosynth_buffer) {
            picosynth_buffer_size = 0;
            return NULL;
        }
        picosynth_buffer_size = num_samples;
    }

    for (uint32_t i = 0; i < num_samples; i++)
        picosynth_buffer[i] = picosynth_process(picosynth_instance);

    return picosynth_buffer;
}

/* Render melody from note/beat arrays */
EMSCRIPTEN_KEEPALIVE
uint32_t picosynth_wasm_render_melody(const uint8_t *notes,
                                      const uint8_t *beats,
                                      uint32_t note_count,
                                      int16_t **out_buffer)
{
    if (!picosynth_instance || !notes || !beats || note_count == 0)
        return 0;

    uint32_t total_samples = 0;
    for (uint32_t i = 0; i < note_count; i++) {
        uint8_t beat = beats[i];
        if (beat == 0)
            beat = 1;
        uint32_t dur = (uint32_t) ((2UL * SAMPLE_RATE) / beat);
        if (total_samples > MAX_BUFFER_SAMPLES - dur)
            return 0;
        total_samples += dur;
    }

    if (total_samples == 0 || total_samples > MAX_BUFFER_SAMPLES)
        return 0;

    int16_t *melody_buffer = malloc(total_samples * sizeof(int16_t));
    if (!melody_buffer)
        return 0;

    uint32_t sample_idx = 0;
    for (uint32_t i = 0; i < note_count; i++) {
        uint8_t note = notes[i];
        uint8_t beat = beats[i];
        if (beat == 0)
            beat = 1;
        uint32_t note_dur = (uint32_t) ((2UL * SAMPLE_RATE) / beat);

        if (note)
            picosynth_wasm_note_on(note);

        uint32_t release_point;
        if (note_dur > 10) {
            release_point = note_dur - note_dur / 5;
            if (release_point >= note_dur)
                release_point = note_dur - 1;
        } else {
            release_point = (note_dur * 4) / 5;
        }

        for (uint32_t j = 0; j < note_dur; j++) {
            if (j == release_point)
                picosynth_wasm_note_off();
            melody_buffer[sample_idx++] = picosynth_process(picosynth_instance);
        }
    }

    if (out_buffer) {
        *out_buffer = melody_buffer;
    } else {
        free(melody_buffer);
    }

    return sample_idx;
}

/* Get sample rate */
EMSCRIPTEN_KEEPALIVE
uint32_t picosynth_wasm_get_sample_rate(void)
{
    return SAMPLE_RATE;
}

/* Convert MIDI note to frequency (for display) */
EMSCRIPTEN_KEEPALIVE
q15_t picosynth_wasm_midi_to_freq(uint8_t note)
{
    return picosynth_midi_to_freq(note);
}
