/**
 * PicoSynth - A lightweight software synthesizer.
 *
 * Modular design: voices contain interconnected nodes (oscillators,
 * envelopes, filters). Nodes wire together via 'q15_t *' pointers.
 *
 * Example:
 *   picosynth_t *s = picosynth_create(2, 8);
 *   picosynth_voice_t *v = picosynth_get_voice(s, 0);
 *   picosynth_node_t *env = picosynth_voice_get_node(v, 0);
 *   picosynth_node_t *osc = picosynth_voice_get_node(v, 1);
 *   picosynth_node_t *flt = picosynth_voice_get_node(v, 2);
 *
 *   picosynth_init_env_ms(env, NULL, 10, 100, 80, 50);
 *   picosynth_init_osc(osc, &env->out, picosynth_voice_freq_ptr(v),
 *                      picosynth_wave_sine);
 *   picosynth_init_lp(flt, NULL, &osc->out, 5000);
 *   picosynth_voice_set_out(v, 2);
 *
 *   picosynth_note_on(s, 0, 60);
 *   q15_t sample = picosynth_process(s);
 *   picosynth_destroy(s);
 */

#ifndef PICOSYNTH_H_
#define PICOSYNTH_H_

#include <stdbool.h>
#include <stdint.h>

#ifndef SAMPLE_RATE
#define SAMPLE_RATE 11025
#endif

/* Block size for envelope processing optimization.
 * Rate computed once per block, transitions checked per-sample.
 * Maximum 255 (uint8_t counter). Typical values: 16, 32, 64.
 */
#ifndef PICOSYNTH_BLOCK_SIZE
#define PICOSYNTH_BLOCK_SIZE 32
#endif

#if PICOSYNTH_BLOCK_SIZE > 255
#error "PICOSYNTH_BLOCK_SIZE must be <= 255 (uint8_t block_counter)"
#endif

/**
 * Q15 fixed-point: signed 16-bit, 15 fractional bits.
 * Range: [-1.0, +1.0) as [-32768, +32767].
 * Multiply: (q15_t)(((int32_t)a * b) >> 15)
 */
typedef int16_t q15_t;
typedef int8_t q7_t;

#define Q15_MAX 0x7FFF /* +0.99997 */
#define Q15_MIN 0x8000 /* -1.0 */

/* Saturating cast from int32_t to q15_t */
static inline q15_t q15_sat(int32_t x)
{
    if (x > Q15_MAX)
        return Q15_MAX;
    if (x < -32768)
        return Q15_MIN;
    return (q15_t) x;
}

/* Waveform generator function pointer */
typedef q15_t (*picosynth_wave_func_t)(q15_t phase);

/* Oscillator state */
typedef struct {
    const q15_t *freq;          /* Phase increment (frequency control) */
    const q15_t *detune;        /* Optional FM/detune offset */
    picosynth_wave_func_t wave; /* Waveform generator (phase -> sample) */
} picosynth_osc_t;

/* ADSR envelope state. Rates are step values scaled <<4 internally.
 * Use synth_init_env_ms().
 */
typedef struct {
    int32_t attack;  /* Ramp-up rate */
    int32_t decay;   /* Ramp-down rate to sustain */
    q15_t sustain;   /* Hold level (negative inverts output) */
    int32_t release; /* Ramp-down rate after note-off */
    /* Block processing state (computed at block boundaries) */
    int32_t block_rate;    /* Current per-sample rate */
    uint8_t block_counter; /* Samples until next rate computation */
} picosynth_env_t;

/* Single-pole filter state */
typedef struct {
    const q15_t *in; /* Input signal pointer */
    int32_t accum;   /* Internal accumulator (Q31) */
    q15_t coeff;     /* Cutoff: 0=DC, Q15_MAX=bypass */
} picosynth_filter_t;

/* 3-input mixer state */
typedef struct {
    const q15_t *in[3]; /* Input signal pointers (NULL = unused) */
} picosynth_mixer_t;

/* Node types */
typedef enum {
    PICOSYNTH_NODE_NONE = 0,
    PICOSYNTH_NODE_OSC,
    PICOSYNTH_NODE_ENV,
    PICOSYNTH_NODE_LP,
    PICOSYNTH_NODE_HP,
    PICOSYNTH_NODE_MIX,
} picosynth_node_type_t;

/* Audio processing node */
typedef struct {
    int32_t state;     /* Internal state (phase, level, etc.) */
    const q15_t *gain; /* Amplitude modulation input */
    q15_t out;         /* Output signal */
    picosynth_node_type_t type;
    union {
        picosynth_osc_t osc;
        picosynth_env_t env;
        picosynth_filter_t flt;
        picosynth_mixer_t mix;
    };
} picosynth_node_t;

/* Opaque types (definitions in picosynth.c) */
typedef struct picosynth_voice picosynth_voice_t;
typedef struct picosynth picosynth_t;

/* Create synthesizer. Returns NULL on failure. Caller must synth_destroy(). */
picosynth_t *picosynth_create(uint8_t voices, uint8_t nodes);

/* Free synthesizer and all resources */
void picosynth_destroy(picosynth_t *s);

/* Get voice by index (NULL if out of bounds) */
picosynth_voice_t *picosynth_get_voice(picosynth_t *s, uint8_t idx);

/* Get node by index within voice (NULL if out of bounds) */
picosynth_node_t *picosynth_voice_get_node(picosynth_voice_t *v, uint8_t idx);

/* Set which node provides voice output (also recomputes usage mask) */
void picosynth_voice_set_out(picosynth_voice_t *v, uint8_t idx);

/* Get pointer to voice's frequency (for wiring to oscillator) */
const q15_t *picosynth_voice_freq_ptr(picosynth_voice_t *v);

/* Trigger note (sets frequency, resets envelopes) */
void picosynth_note_on(picosynth_t *s, uint8_t voice, uint8_t note);

/* Release note (starts envelope release phase) */
void picosynth_note_off(picosynth_t *s, uint8_t voice);

/* Convert MIDI note (0-127) to phase increment */
q15_t picosynth_midi_to_freq(uint8_t note);

/* Initialize oscillator node. Set n->osc.detune after init if needed. */
void picosynth_init_osc(picosynth_node_t *n,
                        const q15_t *gain,
                        const q15_t *freq,
                        picosynth_wave_func_t wave);

/* Initialize envelope node (rates as increments per sample, scaled) */
void picosynth_init_env(picosynth_node_t *n,
                        const q15_t *gain,
                        int32_t attack,
                        int32_t decay,
                        q15_t sustain,
                        int32_t release);

/* Initialize envelope with millisecond timings and percentage sustain */
void picosynth_init_env_ms(picosynth_node_t *n,
                           const q15_t *gain,
                           uint16_t atk_ms,
                           uint16_t dec_ms,
                           uint8_t sus_pct,
                           uint16_t rel_ms);

/* Initialize low-pass filter node */
void picosynth_init_lp(picosynth_node_t *n,
                       const q15_t *gain,
                       const q15_t *in,
                       q15_t coeff);

/* Initialize high-pass filter node */
void picosynth_init_hp(picosynth_node_t *n,
                       const q15_t *gain,
                       const q15_t *in,
                       q15_t coeff);

/* Initialize 3-input mixer node */
void picosynth_init_mix(picosynth_node_t *n,
                        const q15_t *gain,
                        const q15_t *in1,
                        const q15_t *in2,
                        const q15_t *in3);

/* Process one sample (mix all voices, apply soft clipping) */
q15_t picosynth_process(picosynth_t *s);

/* Waveform generators. Input: phase [0, Q15_MAX]. Output: sample [-Q15_MAX,
 * Q15_MAX].
 */
q15_t picosynth_wave_saw(q15_t phase);      /* Rising sawtooth */
q15_t picosynth_wave_square(q15_t phase);   /* Square wave */
q15_t picosynth_wave_triangle(q15_t phase); /* Triangle wave */
q15_t picosynth_wave_falling(q15_t phase);  /* Falling ramp */
q15_t picosynth_wave_exp(q15_t phase);      /* Exponential decay [0, Q15_MAX] */
q15_t picosynth_wave_noise(q15_t phase);    /* White noise (phase ignored) */
q15_t picosynth_wave_sine(q15_t phase);     /* Sine (LUT-based or sinf) */

/* Convert milliseconds to sample count */
#define PICOSYNTH_MS(ms) ((uint32_t) ((long) (ms) * SAMPLE_RATE / 1000))

#endif /* PICOSYNTH_H_ */
