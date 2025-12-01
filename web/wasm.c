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
static q15_t picosynth_detune;

/* Structure to hold configurable synth parameters */
typedef struct {
    /* Voice 0 (Main Tone) */
    picosynth_wave_func_t v0_osc1_wave, v0_osc2_wave;
    uint16_t v0_env_a, v0_env_d;
    q15_t v0_env_s;
    uint16_t v0_env_r;
    q15_t v0_filter_coeff;

    /* Voice 1 (Transient + Body) */
    picosynth_wave_func_t v1_osc1_wave; /* Transient */
    picosynth_wave_func_t v1_osc2_wave; /* Body */
    uint16_t v1_env1_a;                 /* Transient */
    uint16_t v1_env1_d;
    q15_t v1_env1_s;
    uint16_t v1_env1_r;
    uint16_t v1_env2_a; /* Body */
    uint16_t v1_env2_d;
    q15_t v1_env2_s;
    uint16_t v1_env2_r;
    q15_t v1_filter_coeff;
} picosynth_params_t;

/* Global parameters with defaults matching the original piano patch */
static picosynth_params_t picosynth_params = {
    /* Voice 0 */
    .v0_osc1_wave = picosynth_wave_sine,
    .v0_osc2_wave = picosynth_wave_triangle,
    .v0_env_a = 12000,
    .v0_env_d = 350,
    .v0_env_s = (q15_t) (Q15_MAX * 2 / 10),
    .v0_env_r = 50,
    .v0_filter_coeff = 5000,

    /* Voice 1 */
    .v1_osc1_wave = picosynth_wave_saw,
    .v1_osc2_wave = picosynth_wave_triangle,
    .v1_env1_a = 15000,
    .v1_env1_d = 1200,
    .v1_env1_s = (q15_t) (Q15_MAX / 25),
    .v1_env1_r = 30,
    .v1_env2_a = 10000,
    .v1_env2_d = 250,
    .v1_env2_s = (q15_t) (Q15_MAX * 15 / 100),
    .v1_env2_r = 40,
    .v1_filter_coeff = 6500,
};

static picosynth_wave_func_t get_wave_func(uint8_t wave_idx)
{
    switch (wave_idx) {
    case 0:
        return picosynth_wave_sine;
    case 1:
        return picosynth_wave_triangle;
    case 2:
        return picosynth_wave_saw;
    case 3:
        return picosynth_wave_square;
    case 4:
        return picosynth_wave_noise;
    default:
        return picosynth_wave_sine;
    }
}

/* Re-initialize voices based on global parameters */
static void reinit_voices(void)
{
    if (!picosynth_instance)
        return;

    /* Voice 0 */
    picosynth_voice_t *v0 = picosynth_get_voice(picosynth_instance, 0);
    if (v0) {
        picosynth_node_t *flt = picosynth_voice_get_node(v0, 0);
        picosynth_node_t *env = picosynth_voice_get_node(v0, 1);
        picosynth_node_t *osc1 = picosynth_voice_get_node(v0, 2);
        picosynth_node_t *osc2 = picosynth_voice_get_node(v0, 3);
        picosynth_node_t *mix = picosynth_voice_get_node(v0, 4);

        picosynth_init_env(env, NULL, picosynth_params.v0_env_a,
                           picosynth_params.v0_env_d, picosynth_params.v0_env_s,
                           picosynth_params.v0_env_r);
        picosynth_init_osc(osc1, &env->out, picosynth_voice_freq_ptr(v0),
                           picosynth_params.v0_osc1_wave);
        picosynth_init_osc(osc2, &env->out, picosynth_voice_freq_ptr(v0),
                           picosynth_params.v0_osc2_wave);
        osc2->osc.detune = &picosynth_detune;
        picosynth_init_mix(mix, NULL, &osc1->out, &osc2->out, NULL);
        picosynth_init_lp(flt, NULL, &mix->out,
                          picosynth_params.v0_filter_coeff);
        picosynth_voice_set_out(v0, 0);
    }

    /* Voice 1 */
    picosynth_voice_t *v1 = picosynth_get_voice(picosynth_instance, 1);
    if (v1) {
        picosynth_node_t *flt = picosynth_voice_get_node(v1, 0);
        picosynth_node_t *env1 =
            picosynth_voice_get_node(v1, 1); /* Transient */
        picosynth_node_t *osc1 = picosynth_voice_get_node(v1, 2);
        picosynth_node_t *env2 = picosynth_voice_get_node(v1, 3); /* Body */
        picosynth_node_t *osc2 = picosynth_voice_get_node(v1, 4);
        picosynth_node_t *mix = picosynth_voice_get_node(v1, 5);

        picosynth_init_env(
            env1, NULL, picosynth_params.v1_env1_a, picosynth_params.v1_env1_d,
            picosynth_params.v1_env1_s, picosynth_params.v1_env1_r);
        picosynth_init_osc(osc1, &env1->out, picosynth_voice_freq_ptr(v1),
                           picosynth_params.v1_osc1_wave);
        picosynth_init_env(
            env2, NULL, picosynth_params.v1_env2_a, picosynth_params.v1_env2_d,
            picosynth_params.v1_env2_s, picosynth_params.v1_env2_r);
        picosynth_init_osc(osc2, &env2->out, picosynth_voice_freq_ptr(v1),
                           picosynth_params.v1_osc2_wave);
        picosynth_init_mix(mix, NULL, &osc1->out, &osc2->out, NULL);
        picosynth_init_lp(flt, NULL, &mix->out,
                          picosynth_params.v1_filter_coeff);
        picosynth_voice_set_out(v1, 0);
    }
}

/* Initialize synth engine */
EMSCRIPTEN_KEEPALIVE
int picosynth_wasm_init(void)
{
    if (picosynth_instance)
        picosynth_destroy(picosynth_instance);

    picosynth_instance = picosynth_create(2, 8);
    if (!picosynth_instance)
        return 0;

    reinit_voices();
    return 1;
}

/* Parameter setters */

EMSCRIPTEN_KEEPALIVE
void picosynth_wasm_set_wave(uint8_t voice, uint8_t osc, uint8_t wave_idx)
{
    picosynth_wave_func_t wave = get_wave_func(wave_idx);
    if (voice == 0) {
        if (osc == 0)
            picosynth_params.v0_osc1_wave = wave;
        else
            picosynth_params.v0_osc2_wave = wave;
    } else {
        if (osc == 0)
            picosynth_params.v1_osc1_wave = wave;
        else
            picosynth_params.v1_osc2_wave = wave;
    }
    reinit_voices();
}

EMSCRIPTEN_KEEPALIVE
void picosynth_wasm_set_filter_coeff(uint8_t voice, q15_t coeff)
{
    if (voice == 0)
        picosynth_params.v0_filter_coeff = coeff;
    else
        picosynth_params.v1_filter_coeff = coeff;
    reinit_voices();
}

EMSCRIPTEN_KEEPALIVE
void picosynth_wasm_set_env(uint8_t voice,
                            uint8_t env_idx,
                            uint16_t a,
                            uint16_t d,
                            q15_t s,
                            uint16_t r)
{
    if (voice == 0) {
        picosynth_params.v0_env_a = a;
        picosynth_params.v0_env_d = d;
        picosynth_params.v0_env_s = s;
        picosynth_params.v0_env_r = r;
    } else {
        if (env_idx == 0) { /* Transient */
            picosynth_params.v1_env1_a = a;
            picosynth_params.v1_env1_d = d;
            picosynth_params.v1_env1_s = s;
            picosynth_params.v1_env1_r = r;
        } else { /* Body */
            picosynth_params.v1_env2_a = a;
            picosynth_params.v1_env2_d = d;
            picosynth_params.v1_env2_s = s;
            picosynth_params.v1_env2_r = r;
        }
    }
    reinit_voices();
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

/* Trigger note on both voices */
EMSCRIPTEN_KEEPALIVE
void picosynth_wasm_note_on(uint8_t note)
{
    if (!picosynth_instance)
        return;

    picosynth_note_on(picosynth_instance, 0, note);
    picosynth_note_on(picosynth_instance, 1, note);

    /* Update detune based on frequency */
    picosynth_voice_t *v = picosynth_get_voice(picosynth_instance, 0);
    if (v)
        picosynth_detune = *picosynth_voice_freq_ptr(v) / 500;
}

/* Release note on both voices */
EMSCRIPTEN_KEEPALIVE
void picosynth_wasm_note_off(void)
{
    if (!picosynth_instance)
        return;

    picosynth_note_off(picosynth_instance, 0);
    picosynth_note_off(picosynth_instance, 1);
}

/* Maximum buffer size to prevent overflow (60 seconds at 44100 Hz) */
#define MAX_BUFFER_SAMPLES (60UL * 44100UL)

/* Generate samples into internal buffer, return pointer.
 * Returns library-owned buffer; caller must NOT free.
 * Buffer is reused across calls - copy data if needed.
 */
EMSCRIPTEN_KEEPALIVE
int16_t *picosynth_wasm_render(uint32_t num_samples)
{
    if (!picosynth_instance)
        return NULL;

    /* Guard against allocation overflow */
    if (num_samples == 0 || num_samples > MAX_BUFFER_SAMPLES)
        return NULL;

    /* Resize buffer if needed */
    if (num_samples > picosynth_buffer_size) {
        free(picosynth_buffer);
        picosynth_buffer = malloc(num_samples * sizeof(int16_t));
        if (!picosynth_buffer) {
            picosynth_buffer_size = 0;
            return NULL;
        }
        picosynth_buffer_size = num_samples;
    }

    /* Render samples */
    for (uint32_t i = 0; i < num_samples; i++)
        picosynth_buffer[i] = picosynth_process(picosynth_instance);

    return picosynth_buffer;
}

/* Render melody from note/beat arrays, return total samples.
 * Returns caller-owned buffer via out_buffer; caller MUST free with free().
 * If out_buffer is NULL, buffer is freed internally.
 */
EMSCRIPTEN_KEEPALIVE
uint32_t picosynth_wasm_render_melody(const uint8_t *notes,
                                      const uint8_t *beats,
                                      uint32_t note_count,
                                      int16_t **out_buffer)
{
    if (!picosynth_instance || !notes || !beats || note_count == 0)
        return 0;

    /* Calculate total samples needed with overflow protection */
    uint32_t total_samples = 0;
    for (uint32_t i = 0; i < note_count; i++) {
        /* Guard against division by zero */
        uint8_t beat = beats[i];
        if (beat == 0)
            beat = 1;
        uint32_t dur = (uint32_t) ((2UL * SAMPLE_RATE) / beat);
        /* Check for overflow before adding */
        if (total_samples > MAX_BUFFER_SAMPLES - dur)
            return 0;
        total_samples += dur;
    }

    /* Final overflow check */
    if (total_samples == 0 || total_samples > MAX_BUFFER_SAMPLES)
        return 0;

    /* Allocate a new buffer for the melody */
    int16_t *melody_buffer = malloc(total_samples * sizeof(int16_t));
    if (!melody_buffer) {
        return 0;
    }

    /* Render melody */
    uint32_t sample_idx = 0;
    for (uint32_t i = 0; i < note_count; i++) {
        uint8_t note = notes[i];
        /* Guard against division by zero */
        uint8_t beat = beats[i];
        if (beat == 0)
            beat = 1;
        uint32_t note_dur = (uint32_t) ((2UL * SAMPLE_RATE) / beat);

        if (note)
            picosynth_wasm_note_on(note);

        /* Calculate release point (80% of note duration)
         * Ensure release_point < note_dur so note_off always fires */
        uint32_t release_point;
        if (note_dur > 10) {
            release_point = note_dur - note_dur / 5;
            /* Ensure at least 1 sample gap for note_off to trigger */
            if (release_point >= note_dur)
                release_point = note_dur - 1;
        } else {
            /* Very short notes: release at 80% but at least sample 0 */
            release_point = (note_dur * 4) / 5;
        }

        for (uint32_t j = 0; j < note_dur; j++) {
            /* Release before end for natural decay */
            if (j == release_point)
                picosynth_wasm_note_off();
            melody_buffer[sample_idx++] = picosynth_process(picosynth_instance);
        }
    }

    if (out_buffer) {
        *out_buffer = melody_buffer;
    } else {
        /* Caller doesn't want the buffer, free it to avoid leak */
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
