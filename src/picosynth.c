/* Lightweight software synthesizer */

#include <stdlib.h>
#include <string.h>

#include "dsp-math.h"
#include "picosynth.h"

/* Envelope state: bits 30-31 = mode, bits 0-29 = value
 * Mode 0 (0x00): Attack - ramp up to peak
 * Mode 1 (0x40): Hold - maintain peak level
 * Mode 2 (0x80): Decay/Sustain - exponential decay to sustain
 */
#define ENVELOPE_MODE_ATTACK 0x00000000u
#define ENVELOPE_MODE_HOLD 0x40000000u
#define ENVELOPE_MODE_DECAY 0x80000000u
#define ENVELOPE_MODE_MASK 0xC0000000u
#define ENVELOPE_STATE_VALUE_MASK 0x3FFFFFFF

/* DC blocker coefficient: alpha ≈ 0.995 in Q15 = 32604
 * Removes DC offset from waveshaping with ~4 cycles/sample overhead.
 */
#define DC_BLOCK_ALPHA 32604

/* Opaque type definitions */
struct picosynth_voice {
    uint8_t note;            /* Current MIDI note */
    uint8_t gate : 1;        /* 1=key held, 0=released */
    uint8_t out_idx;         /* Output node index */
    uint8_t node_usage_mask; /* Bit N = node N affects output */
    q15_t freq;              /* Base frequency (phase increment) */
    picosynth_node_t *nodes;
    uint8_t n_nodes;
};

struct picosynth {
    picosynth_voice_t *voices;
    uint8_t num_voices;
    uint16_t voice_enable_mask; /* Bit N = voice N active */
    /* DC blocker state (placed after main mixer, before soft clipper) */
    int32_t dc_x_prev, dc_y_prev; /* Previous input, output */
};

/* LFSR seed for noise generator.
 * Global state shared across all instances - not thread-safe.
 * For multi-threaded use, move seed into picosynth_t and change
 * picosynth_wave_noise() to accept instance pointer.
 */
static uint32_t lfsr_seed = 0x12345678;

static void voice_note_on(picosynth_voice_t *v, uint8_t note)
{
    v->note = note;
    v->gate = 1;
    v->freq = picosynth_midi_to_freq(note);
    for (int i = 0; i < v->n_nodes; i++) {
        picosynth_node_t *n = &v->nodes[i];
        n->state = 0;
        n->out = 0;
        /* Reset filter accumulators to prevent DC offsets/pops */
        if (n->type == PICOSYNTH_NODE_LP || n->type == PICOSYNTH_NODE_HP) {
            n->flt.accum = 0;
            n->flt.coeff = n->flt.coeff_target;
        }
        /* Reset SVF states */
        if (n->type == PICOSYNTH_NODE_SVF_LP ||
            n->type == PICOSYNTH_NODE_SVF_HP ||
            n->type == PICOSYNTH_NODE_SVF_BP) {
            n->svf.lp = 0;
            n->svf.bp = 0;
            n->svf.f = n->svf.f_target;
        }
        /* Reset envelope block state to force immediate rate calculation */
        if (n->type == PICOSYNTH_NODE_ENV) {
            n->env.block_counter = 0;
            n->env.block_rate = 0;
            n->env.hold_counter = 0;
        }
    }
}

static void voice_note_off(picosynth_voice_t *v)
{
    v->gate = 0;
    /* Force immediate rate recalculation for all envelope nodes.
     * Without this, envelopes continue at their previous rate (attack/decay)
     * until the next block boundary, causing audible pops. */
    for (int i = 0; i < v->n_nodes; i++) {
        picosynth_node_t *n = &v->nodes[i];
        if (n->type == PICOSYNTH_NODE_ENV)
            n->env.block_counter = 0;
    }
}

/* Map a pointer to a node's output field back to the node index.
 * Returns -1 if the pointer doesn't point to any node's output.
 */
static int ptr_to_node_idx(picosynth_voice_t *v, const q15_t *ptr)
{
    if (!ptr)
        return -1;

    /* Check if ptr points to any node's out field */
    for (int i = 0; i < v->n_nodes; i++) {
        if (ptr == &v->nodes[i].out)
            return i;
    }
    return -1; /* External pointer (e.g., voice freq) */
}

/* Recursively mark node and all its dependencies as used.
 * Note: mask is 8-bit, so only nodes 0-7 can be tracked. For voices with >8
 * nodes, the mask optimization is disabled (mask stays 0, all nodes processed).
 */
static void mark_node_used(picosynth_voice_t *v, int idx)
{
    if (idx < 0 || idx >= v->n_nodes)
        return;
    /* Skip nodes >= 8 to avoid UB from oversized shift. If any node >= 8 is
     * in the dependency chain, we can't use the optimization safely. */
    if (idx >= 8) {
        v->node_usage_mask = 0; /* Disable optimization */
        return;
    }

    uint8_t bit = (uint8_t) (1u << idx);
    if (v->node_usage_mask & bit)
        return; /* Already marked, avoid infinite recursion */

    v->node_usage_mask |= bit;
    picosynth_node_t *n = &v->nodes[idx];

    /* Trace gain input (common to all node types) */
    int dep = ptr_to_node_idx(v, n->gain);
    if (dep >= 0)
        mark_node_used(v, dep);

    /* Trace type-specific inputs */
    switch (n->type) {
    case PICOSYNTH_NODE_OSC:
        /* freq typically points to voice->freq, not a node */
        dep = ptr_to_node_idx(v, n->osc.freq);
        if (dep >= 0)
            mark_node_used(v, dep);
        dep = ptr_to_node_idx(v, n->osc.detune);
        if (dep >= 0)
            mark_node_used(v, dep);
        break;
    case PICOSYNTH_NODE_LP:
    case PICOSYNTH_NODE_HP:
        dep = ptr_to_node_idx(v, n->flt.in);
        if (dep >= 0)
            mark_node_used(v, dep);
        break;
    case PICOSYNTH_NODE_SVF_LP:
    case PICOSYNTH_NODE_SVF_HP:
    case PICOSYNTH_NODE_SVF_BP:
        dep = ptr_to_node_idx(v, n->svf.in);
        if (dep >= 0)
            mark_node_used(v, dep);
        break;
    case PICOSYNTH_NODE_MIX:
        for (int j = 0; j < 3; j++) {
            dep = ptr_to_node_idx(v, n->mix.in[j]);
            if (dep >= 0)
                mark_node_used(v, dep);
        }
        break;
    default:
        break;
    }
}

/* Update node usage mask by tracing dependencies from output node.
 * Call this after wiring nodes, before processing.
 */
static void voice_update_usage_mask(picosynth_voice_t *v)
{
    v->node_usage_mask = 0;
    if (v->out_idx < v->n_nodes)
        mark_node_used(v, v->out_idx);
}

picosynth_t *picosynth_create(uint8_t voices, uint8_t nodes)
{
    if (nodes > PICOSYNTH_MAX_NODES)
        return NULL;

    picosynth_t *s = calloc(1, sizeof(picosynth_t));
    if (!s)
        return NULL;

    s->num_voices = voices;
    s->voices = calloc(voices, sizeof(picosynth_voice_t));
    if (!s->voices) {
        free(s);
        return NULL;
    }

    for (int i = 0; i < voices; i++) {
        s->voices[i].n_nodes = nodes;
        s->voices[i].nodes = calloc(nodes, sizeof(picosynth_node_t));
        if (!s->voices[i].nodes) {
            for (int j = 0; j < i; j++)
                free(s->voices[j].nodes);
            free(s->voices);
            free(s);
            return NULL;
        }
    }
    return s;
}

void picosynth_destroy(picosynth_t *s)
{
    if (!s)
        return;

    for (int i = 0; i < s->num_voices; i++)
        free(s->voices[i].nodes);
    free(s->voices);
    free(s);
}

picosynth_voice_t *picosynth_get_voice(picosynth_t *s, uint8_t idx)
{
    if (!s || idx >= s->num_voices)
        return NULL;
    return &s->voices[idx];
}

picosynth_node_t *picosynth_voice_get_node(picosynth_voice_t *v, uint8_t idx)
{
    if (!v || idx >= v->n_nodes)
        return NULL;
    return &v->nodes[idx];
}

void picosynth_voice_set_out(picosynth_voice_t *v, uint8_t idx)
{
    if (v && idx < v->n_nodes) {
        v->out_idx = idx;
        voice_update_usage_mask(v);
    }
}

const q15_t *picosynth_voice_freq_ptr(picosynth_voice_t *v)
{
    if (!v)
        return NULL;
    return &v->freq;
}

void picosynth_note_on(picosynth_t *s, uint8_t voice, uint8_t note)
{
    if (s && voice < s->num_voices) {
        voice_note_on(&s->voices[voice], note);
        /* Only track in bitmask for voices 0-15 (16-bit mask) */
        if (voice < 16)
            s->voice_enable_mask |= (uint16_t) (1u << voice);
    }
}

void picosynth_note_off(picosynth_t *s, uint8_t voice)
{
    if (s && voice < s->num_voices)
        voice_note_off(&s->voices[voice]);
}

/* Internal macros for frequency and envelope rate calculations */
#define PICOSYNTH_HZ_TO_FREQ(hz) \
    ((q15_t) (((long) (hz) * Q15_MAX) / SAMPLE_RATE))
#define PICOSYNTH_ENV_RATE_FROM_MS(ms)                                    \
    (PICOSYNTH_MS(ms) > 0 ? (((int64_t) Q15_MAX << 4) / PICOSYNTH_MS(ms)) \
                          : ((int32_t) Q15_MAX << 4))
#define PICOSYNTH_ENV_MIN_RATIO_Q15 \
    ((q15_t) (((int64_t) Q15_MAX + 5000) / 10000)) /* ~1e-4 */
#define PICOSYNTH_ENV_MAX_RATIO_Q15 \
    ((q15_t) (((int64_t) Q15_MAX * 9999 + 5000) / 10000)) /* 0.9999 */
/* Minimum release duration to avoid clicks when retriggering notes */
#define PICOSYNTH_FAST_RELEASE_SAMPLES (SAMPLE_RATE / 100) /* ~10ms */

/* Phase increments for octave 8; shift right for lower octaves */
#define BASE_OCTAVE 8
#define NOTES_PER_OCTAVE 12
static const q15_t octave8_freq[NOTES_PER_OCTAVE] = {
    PICOSYNTH_HZ_TO_FREQ(4186.01), /* C8 */
    PICOSYNTH_HZ_TO_FREQ(4434.92), /* C#8 */
    PICOSYNTH_HZ_TO_FREQ(4698.63), /* D8 */
    PICOSYNTH_HZ_TO_FREQ(4978.03), /* D#8 */
    PICOSYNTH_HZ_TO_FREQ(5274.04), /* E8 */
    PICOSYNTH_HZ_TO_FREQ(5587.65), /* F8 */
    PICOSYNTH_HZ_TO_FREQ(5919.91), /* F#8 */
    PICOSYNTH_HZ_TO_FREQ(6271.93), /* G8 */
    PICOSYNTH_HZ_TO_FREQ(6644.88), /* G#8 */
    PICOSYNTH_HZ_TO_FREQ(7040.00), /* A8 */
    PICOSYNTH_HZ_TO_FREQ(7458.62), /* A#8 */
    PICOSYNTH_HZ_TO_FREQ(7902.13), /* B8 */
};

/* Multiply Q1.15 values with 64-bit intermediate to preserve precision */
static q15_t q15_mul(q15_t a, q15_t b)
{
    return (q15_t) (((int64_t) a * b) >> 15);
}

/* Fast exponentiation: compute base^exp in Q1.15 domain. */
static q15_t pow_q15(q15_t base, uint32_t exp)
{
    int32_t result = Q15_MAX; /* 1.0 */
    int32_t b = base;
    while (exp) {
        if (exp & 1u)
            result = (int32_t) q15_mul((q15_t) result, (q15_t) b);
        exp >>= 1;
        if (exp)
            b = (int32_t) q15_mul((q15_t) b, (q15_t) b);
    }
    return (q15_t) result;
}

/* Calculate exponential envelope multiplier for a given duration in samples
 * toward a target ratio relative to the starting value.
 */
static q15_t env_calc_exp_coeff(uint32_t samples, q15_t target_ratio)
{
    if (samples < 10)
        return Q15_MAX >> 1; /* Very fast decay */

    if (target_ratio < PICOSYNTH_ENV_MIN_RATIO_Q15)
        target_ratio = PICOSYNTH_ENV_MIN_RATIO_Q15;
    if (target_ratio > PICOSYNTH_ENV_MAX_RATIO_Q15)
        target_ratio = PICOSYNTH_ENV_MAX_RATIO_Q15;

    int32_t low = 0, high = Q15_MAX;
    while (low + 1 < high) {
        int32_t mid = (low + high) >> 1;
        q15_t pow_mid = pow_q15((q15_t) mid, samples);
        if (pow_mid > target_ratio)
            high = mid;
        else
            low = mid;
    }

    /* Choose the closer bound */
    q15_t pow_low = pow_q15((q15_t) low, samples);
    q15_t pow_high = pow_q15((q15_t) high, samples);
    int32_t diff_low = (int32_t) target_ratio - (int32_t) pow_low;
    int32_t diff_high = (int32_t) pow_high - (int32_t) target_ratio;
    if (diff_low < 0)
        diff_low = -diff_low;
    if (diff_high < 0)
        diff_high = -diff_high;
    return diff_low <= diff_high ? (q15_t) low : (q15_t) high;
}

/* Recalculate decay/release exponential coefficients to roughly match the
 * linear timing implied by the configured rates.
 */
static void env_update_exp_coeffs(picosynth_env_t *env)
{
    int32_t sus_abs = env->sustain < 0 ? -env->sustain : env->sustain;
    uint32_t sus_level = (uint32_t) sus_abs << 4;
    uint32_t peak = (uint32_t) Q15_MAX << 4;
    uint32_t decay_span = peak > sus_level ? peak - sus_level : 1;

    uint32_t decay_samples =
        env->decay > 0
            ? (decay_span + (uint32_t) env->decay - 1) / (uint32_t) env->decay
            : 1;
    q15_t target = (q15_t) (((int64_t) sus_level << 15) / peak);
    env->decay_coeff = env_calc_exp_coeff(decay_samples, target);

    uint32_t release_samples =
        env->release > 0
            ? (peak + (uint32_t) env->release - 1) / (uint32_t) env->release
            : 1;
    if (release_samples < PICOSYNTH_FAST_RELEASE_SAMPLES)
        release_samples = PICOSYNTH_FAST_RELEASE_SAMPLES;
    env->release_coeff =
        env_calc_exp_coeff(release_samples, PICOSYNTH_ENV_MIN_RATIO_Q15);
}

q15_t picosynth_midi_to_freq(uint8_t note)
{
    if (note > 119)
        note = 119; /* Clamp to B9 */

    int octave = note / NOTES_PER_OCTAVE;
    int idx = note % NOTES_PER_OCTAVE;
    int shift = BASE_OCTAVE - octave;
    if (shift >= 0)
        return octave8_freq[idx] >> shift;

    /* Octave 9+: left-shift with saturation to prevent overflow */
    int32_t f = (int32_t) octave8_freq[idx] << (-shift);
    return q15_sat(f);
}

void picosynth_init_osc(picosynth_node_t *n,
                        const q15_t *gain,
                        const q15_t *freq,
                        picosynth_wave_func_t wave)
{
    memset(n, 0, sizeof(picosynth_node_t));
    n->gain = gain;
    n->type = PICOSYNTH_NODE_OSC;
    n->osc.freq = freq;
    n->osc.wave = wave;
}

void picosynth_init_env(picosynth_node_t *n,
                        const q15_t *gain,
                        const picosynth_env_params_t *params)
{
    memset(n, 0, sizeof(picosynth_node_t));
    n->gain = gain;
    n->type = PICOSYNTH_NODE_ENV;
    n->env.attack = params->attack;
    n->env.hold = params->hold;
    n->env.decay = params->decay;
    n->env.sustain = params->sustain;
    n->env.release = params->release;
    n->env.hold_counter = 0;
    env_update_exp_coeffs(&n->env);
}

void picosynth_init_env_ms(picosynth_node_t *n,
                           const q15_t *gain,
                           const picosynth_env_ms_params_t *params)
{
    picosynth_env_params_t p = {
        .attack = (int32_t) PICOSYNTH_ENV_RATE_FROM_MS(params->atk_ms),
        .hold = (int32_t) PICOSYNTH_MS(params->hold_ms),
        .decay = (int32_t) PICOSYNTH_ENV_RATE_FROM_MS(params->dec_ms),
        .sustain = (q15_t) (((int32_t) params->sus_pct * Q15_MAX) / 100),
        .release = (int32_t) PICOSYNTH_ENV_RATE_FROM_MS(params->rel_ms),
    };
    picosynth_init_env(n, gain, &p);
}

void picosynth_init_lp(picosynth_node_t *n,
                       const q15_t *gain,
                       const q15_t *in,
                       q15_t coeff)
{
    memset(n, 0, sizeof(picosynth_node_t));
    n->gain = gain;
    n->type = PICOSYNTH_NODE_LP;
    n->flt.in = in;
    n->flt.coeff = coeff;
    n->flt.coeff_target = coeff;
}

void picosynth_init_hp(picosynth_node_t *n,
                       const q15_t *gain,
                       const q15_t *in,
                       q15_t coeff)
{
    memset(n, 0, sizeof(picosynth_node_t));
    n->gain = gain;
    n->type = PICOSYNTH_NODE_HP;
    n->flt.in = in;
    n->flt.coeff = coeff;
    n->flt.coeff_target = coeff;
}

void picosynth_filter_set_coeff(picosynth_node_t *n, q15_t coeff)
{
    if (!n || (n->type != PICOSYNTH_NODE_LP && n->type != PICOSYNTH_NODE_HP))
        return;
    n->flt.coeff_target = q15_sat(coeff);
}

q15_t picosynth_svf_freq(uint16_t fc_hz)
{
    /* f = 2 * sin(pi * fc / fs)
     * For stability, fc should be < fs/4 (Nyquist/2)
     * Map fc_hz to table index: idx = fc_hz * 64 / SAMPLE_RATE
     */
    if (fc_hz == 0)
        return 0;

    /* Clamp to safe range (fs/4 max for stability) */
    uint32_t max_fc = SAMPLE_RATE / 4;
    if (fc_hz > max_fc)
        fc_hz = (uint16_t) max_fc;

    /* Calculate table index with interpolation
     * idx = fc * 64 / fs, but we use fc * 64 * 256 / fs for fractional part
     */
    uint32_t scaled = ((uint32_t) fc_hz * 64 * 256) / SAMPLE_RATE;
    uint8_t idx = (uint8_t) (scaled >> 8);
    uint8_t frac = (uint8_t) (scaled & 0xFF);

    if (idx >= 32) {
        idx = 32;
        frac = 0;
    }

    /* Linear interpolation between table entries */
    int32_t s0 = svf_sin_table[idx];
    int32_t s1 = svf_sin_table[idx + 1];
    int32_t sin_val = s0 + (((s1 - s0) * frac) >> 8);

    /* f = 2 * sin(...), but we limit to avoid instability */
    int32_t f = sin_val * 2;
    if (f > Q15_MAX)
        f = Q15_MAX;

    return (q15_t) f;
}

void picosynth_init_svf_lp(picosynth_node_t *n,
                           const q15_t *gain,
                           const q15_t *in,
                           q15_t f_coeff,
                           q15_t q)
{
    memset(n, 0, sizeof(picosynth_node_t));
    n->gain = gain;
    n->type = PICOSYNTH_NODE_SVF_LP;
    n->svf.in = in;
    n->svf.f = f_coeff;
    n->svf.f_target = f_coeff;
    n->svf.q = q;
    n->svf.lp = 0;
    n->svf.bp = 0;
}

void picosynth_init_svf_hp(picosynth_node_t *n,
                           const q15_t *gain,
                           const q15_t *in,
                           q15_t f_coeff,
                           q15_t q)
{
    memset(n, 0, sizeof(picosynth_node_t));
    n->gain = gain;
    n->type = PICOSYNTH_NODE_SVF_HP;
    n->svf.in = in;
    n->svf.f = f_coeff;
    n->svf.f_target = f_coeff;
    n->svf.q = q;
    n->svf.lp = 0;
    n->svf.bp = 0;
}

void picosynth_init_svf_bp(picosynth_node_t *n,
                           const q15_t *gain,
                           const q15_t *in,
                           q15_t f_coeff,
                           q15_t q)
{
    memset(n, 0, sizeof(picosynth_node_t));
    n->gain = gain;
    n->type = PICOSYNTH_NODE_SVF_BP;
    n->svf.in = in;
    n->svf.f = f_coeff;
    n->svf.f_target = f_coeff;
    n->svf.q = q;
    n->svf.lp = 0;
    n->svf.bp = 0;
}

void picosynth_svf_set_freq(picosynth_node_t *n, q15_t f_coeff)
{
    if (!n)
        return;
    if (n->type != PICOSYNTH_NODE_SVF_LP && n->type != PICOSYNTH_NODE_SVF_HP &&
        n->type != PICOSYNTH_NODE_SVF_BP)
        return;
    n->svf.f_target = f_coeff;
}

void picosynth_init_mix(picosynth_node_t *n,
                        const q15_t *gain,
                        const q15_t *in1,
                        const q15_t *in2,
                        const q15_t *in3)
{
    memset(n, 0, sizeof(picosynth_node_t));
    n->gain = gain;
    n->type = PICOSYNTH_NODE_MIX;
    n->mix.in[0] = in1;
    n->mix.in[1] = in2;
    n->mix.in[2] = in3;
}

static q15_t soft_clip(int32_t x)
{
    int32_t sign = x < 0 ? -1 : 1;
    uint32_t abs_x = x < 0 ? (uint32_t) (-(int64_t) x) : (uint32_t) x;
    uint32_t a = abs_x >> 3;
    if (a > Q15_MAX / 4)
        a = Q15_MAX / 4;
    return q15_sat(picosynth_sine_impl((q15_t) a) * sign);
}

q15_t picosynth_process(picosynth_t *s)
{
    if (!s)
        return 0;

    int32_t out = 0;

    for (int vi = 0; vi < s->num_voices; vi++) {
        /* Skip inactive voices via bitfield check (voices 0-15 only) */
        if (vi < 16 && !(s->voice_enable_mask & (1u << vi)))
            continue;

        picosynth_voice_t *v = &s->voices[vi];
        picosynth_node_t *nodes = v->nodes;
        int32_t tmp[PICOSYNTH_MAX_NODES];

        /* Two-pass processing per voice:
         * 1. Compute outputs from current state of all nodes.
         *    This ensures inputs for a node (e.g. filter) are based on the
         *    outputs of other nodes (e.g. oscillator) from the same sample.
         * 2. Update the internal state of all nodes for the next sample.
         *    This prevents race conditions where a node's state is updated
         *    before its output has been consumed by other nodes.
         */

        /* Pass 1: compute outputs from current state */
        uint8_t mask = v->node_usage_mask;
        for (int i = 0; i < v->n_nodes && nodes[i].type != PICOSYNTH_NODE_NONE;
             i++) {
            /* Skip nodes that don't affect output (if mask is set) */
            if (mask && !(mask & (1u << i))) {
                tmp[i] = 0;
                continue;
            }
            picosynth_node_t *n = &nodes[i];
            switch (n->type) {
            case PICOSYNTH_NODE_OSC:
                tmp[i] = n->osc.wave(n->state & Q15_MAX);
                break;
            case PICOSYNTH_NODE_ENV:
                /* Envelope output is scaled down and squared for a non-linear
                 * curve.
                 */
                tmp[i] = (n->state & ENVELOPE_STATE_VALUE_MASK) >> 4;
                tmp[i] = (tmp[i] * tmp[i]) >> 15; /* Squared curve */
                if (n->env.sustain < 0)
                    tmp[i] = -tmp[i];
                break;
            case PICOSYNTH_NODE_LP:
                tmp[i] =
                    (int32_t) (((int64_t) n->flt.accum * n->flt.coeff) >> 15);
                break;
            case PICOSYNTH_NODE_HP:
                /* High-pass is the input signal minus the low-pass signal */
                if (n->flt.in) {
                    tmp[i] =
                        (int32_t) (((int64_t) n->flt.accum * n->flt.coeff) >>
                                   15);
                    tmp[i] = *n->flt.in - tmp[i];
                } else {
                    tmp[i] = 0;
                }
                break;
            case PICOSYNTH_NODE_SVF_LP:
                /* SVF low-pass output: scaled down from internal precision */
                tmp[i] = n->svf.lp >> 8;
                break;
            case PICOSYNTH_NODE_SVF_HP: {
                /* SVF high-pass: hp = in - lp - q*bp
                 * Computed at internal <<8 scale for consistency with state,
                 * then scaled down. Uses int64_t to prevent overflow.
                 */
                int64_t in_val = n->svf.in ? ((int32_t) *n->svf.in) << 8 : 0;
                int64_t q_bp = ((int64_t) n->svf.bp * n->svf.q) >> 15;
                int64_t hp = in_val - n->svf.lp - q_bp;
                /* Clamp to int32_t range before scaling */
                if (hp > 0x7FFFFFFF)
                    hp = 0x7FFFFFFF;
                else if (hp < (-0x7FFFFFFF - 1))
                    hp = (-0x7FFFFFFF - 1);
                tmp[i] = (int32_t) (hp >> 8);
                break;
            }
            case PICOSYNTH_NODE_SVF_BP:
                /* SVF band-pass output */
                tmp[i] = n->svf.bp >> 8;
                break;
            case PICOSYNTH_NODE_MIX: {
                int32_t sum = 0;
                for (int j = 0; j < 3; j++)
                    if (n->mix.in[j])
                        sum += *n->mix.in[j];
                tmp[i] = sum;
                break;
            }
            default:
                tmp[i] = 0;
                break;
            }

            if (n->gain)
                tmp[i] = (int32_t) (((int64_t) tmp[i] * *n->gain) >> 15);
        }

        /* Pass 2: update state for next sample */
        for (int i = 0; i < v->n_nodes && nodes[i].type != PICOSYNTH_NODE_NONE;
             i++) {
            /* Skip nodes that don't affect output (if mask is set) */
            if (mask && !(mask & (1u << i)))
                continue;
            picosynth_node_t *n = &nodes[i];
            n->out = q15_sat(tmp[i]);

            switch (n->type) {
            case PICOSYNTH_NODE_OSC:
                if (n->osc.freq)
                    n->state += *n->osc.freq;
                if (n->osc.detune)
                    n->state += *n->osc.detune;
                n->state =
                    (int32_t) (((uint32_t) n->state) & (uint32_t) Q15_MAX);
                break;
            case PICOSYNTH_NODE_ENV: {
                /* Block-based AHDSR envelope: compute rate at block
                 * boundaries, check for phase transitions per-sample. */
                uint32_t mode = ((uint32_t) n->state) & ENVELOPE_MODE_MASK;

                /* Recompute rate at block boundary */
                if (n->env.block_counter == 0) {
                    n->env.block_counter = PICOSYNTH_BLOCK_SIZE;
                    if (!v->gate) {
                        n->env.block_rate = -n->env.release; /* Informational */
                    } else if (mode == ENVELOPE_MODE_DECAY) {
                        n->env.block_rate = -n->env.decay; /* Informational */
                    } else if (mode == ENVELOPE_MODE_HOLD) {
                        n->env.block_rate = 0; /* Hold at peak */
                    } else {
                        n->env.block_rate = n->env.attack;
                    }
                }
                n->env.block_counter--;

                /* Apply rate based on mode */
                int32_t val = n->state & ENVELOPE_STATE_VALUE_MASK;
                if (v->gate) {
                    if (mode == ENVELOPE_MODE_DECAY) {
                        /* Decay/Sustain phase */
                        q15_t sus_abs = n->env.sustain < 0 ? -n->env.sustain
                                                           : n->env.sustain;
                        int32_t sus_level = sus_abs << 4;
                        int32_t delta = val - sus_level;
                        /* Exponential decay of delta toward sustain */
                        val =
                            sus_level +
                            (int32_t) (((int64_t) delta * n->env.decay_coeff) >>
                                       15);
                        if (val < sus_level)
                            val = sus_level;
                    } else if (mode == ENVELOPE_MODE_HOLD) {
                        /* Hold phase: maintain peak, count down */
                        val = (int32_t) Q15_MAX << 4; /* Stay at peak */
                        if (n->env.hold_counter > 0)
                            n->env.hold_counter--;
                        if (n->env.hold_counter == 0) {
                            /* Transition to decay mode */
                            mode = ENVELOPE_MODE_DECAY;
                            n->env.block_counter = 0;
                        }
                    } else {
                        /* Attack phase: ramp up to peak */
                        val += n->env.block_rate;
                        if (val >= (int32_t) Q15_MAX << 4) {
                            val = (int32_t) Q15_MAX << 4;
                            /* Check if hold phase is configured */
                            if (n->env.hold > 0) {
                                mode = ENVELOPE_MODE_HOLD;
                                n->env.hold_counter = n->env.hold;
                            } else {
                                mode = ENVELOPE_MODE_DECAY;
                            }
                            /* Force rate recalculation next sample */
                            n->env.block_counter = 0;
                        }
                    }
                    n->state = (int32_t) (((uint32_t) val) | mode);
                } else {
                    /* Exponential release (mode cleared) */
                    val = (int32_t) (((int64_t) val * n->env.release_coeff) >>
                                     15);
                    if (val < 16)
                        val = 0;
                    n->state = val; /* mode bits clear during release */
                }
                break;
            }
            case PICOSYNTH_NODE_LP:
            case PICOSYNTH_NODE_HP: {
                /* Smooth cutoff changes to avoid zipper noise.
                 * Time constant: ~256 samples (~23ms @ 11kHz, ~6ms @ 44kHz).
                 */
                int32_t coeff_delta =
                    (int32_t) n->flt.coeff_target - n->flt.coeff;
                if (coeff_delta) {
                    int32_t step = coeff_delta >> 8;
                    if (step == 0)
                        step = coeff_delta > 0 ? 1 : -1;
                    n->flt.coeff = q15_sat((int32_t) n->flt.coeff + step);
                }

                /* Single-pole filter accumulator update:
                 * accum += (input - output)
                 * where output is the filtered signal from the previous sample.
                 * This implements a simple recursive filter.
                 */
                int32_t input_val = n->flt.in ? *n->flt.in : 0;
                int32_t delta = input_val - n->out;
                int64_t acc = (int64_t) n->flt.accum + delta;
                if (acc > 0x7FFFFFFF)
                    acc = 0x7FFFFFFF;
                else if (acc < (-0x7FFFFFFF - 1))
                    acc = (-0x7FFFFFFF - 1);
                n->flt.accum = (int32_t) acc;
                break;
            }
            case PICOSYNTH_NODE_SVF_LP:
            case PICOSYNTH_NODE_SVF_HP:
            case PICOSYNTH_NODE_SVF_BP: {
                /* Smooth frequency changes to avoid zipper noise */
                int32_t f_delta = (int32_t) n->svf.f_target - n->svf.f;
                if (f_delta) {
                    int32_t step = f_delta >> 8;
                    if (step == 0)
                        step = f_delta > 0 ? 1 : -1;
                    n->svf.f = q15_sat((int32_t) n->svf.f + step);
                }

                /* State Variable Filter update:
                 * hp = in - lp - q*bp
                 * lp_new = lp + f*bp
                 * bp_new = bp + f*hp
                 *
                 * States stored with <<8 scaling for precision.
                 */
                int32_t in_val = n->svf.in ? ((int32_t) *n->svf.in) << 8 : 0;
                int32_t lp = n->svf.lp;
                int32_t bp = n->svf.bp;

                /* Compute high-pass: hp = in - lp - q*bp
                 * Uses int64_t to prevent overflow with large <<8 scaled
                 * values.
                 */
                int64_t q_bp = ((int64_t) bp * n->svf.q) >> 15;
                int64_t hp64 = (int64_t) in_val - lp - q_bp;
                if (hp64 > 0x7FFFFFFF)
                    hp64 = 0x7FFFFFFF;
                else if (hp64 < (-0x7FFFFFFF - 1))
                    hp64 = (-0x7FFFFFFF - 1);
                int32_t hp = (int32_t) hp64;

                /* Update low-pass: lp += f*bp */
                int32_t f_bp = (int32_t) (((int64_t) bp * n->svf.f) >> 15);
                int64_t lp_new = (int64_t) lp + f_bp;
                if (lp_new > 0x7FFFFFFF)
                    lp_new = 0x7FFFFFFF;
                else if (lp_new < (-0x7FFFFFFF - 1))
                    lp_new = (-0x7FFFFFFF - 1);
                n->svf.lp = (int32_t) lp_new;

                /* Update band-pass: bp += f*hp */
                int32_t f_hp = (int32_t) (((int64_t) hp * n->svf.f) >> 15);
                int64_t bp_new = (int64_t) bp + f_hp;
                if (bp_new > 0x7FFFFFFF)
                    bp_new = 0x7FFFFFFF;
                else if (bp_new < (-0x7FFFFFFF - 1))
                    bp_new = (-0x7FFFFFFF - 1);
                n->svf.bp = (int32_t) bp_new;
                break;
            }
            default:
                break;
            }
        }

        out += v->nodes[v->out_idx].out;

        /* Disable voice when fully silent (gate off, all envelopes at zero).
         * Only applies to voices 0-15 tracked by 16-bit mask. */
        if (vi < 16 && !v->gate) {
            bool silent = true;
            for (int i = 0; i < v->n_nodes && silent; i++) {
                if (nodes[i].type == PICOSYNTH_NODE_ENV &&
                    (nodes[i].state & ENVELOPE_STATE_VALUE_MASK) != 0)
                    silent = false;
            }
            if (silent)
                s->voice_enable_mask &= (uint16_t) ~(1u << vi);
        }
    }

    if (s->num_voices > 1) {
        q15_t gain = Q15_MAX / s->num_voices;
        out = (int32_t) (((int64_t) out * gain) >> 15);
    }

    /* DC blocker: y[n] = x[n] - x[n-1] + alpha * y[n-1]
     * Removes DC offset introduced by waveshaping and asymmetric waveforms.
     * Uses int64_t to prevent overflow, clamps state. No rounding on feedback
     * (truncation ensures full decay to zero, rounding leaves residual DC).
     */
    int64_t delta = (int64_t) out - (int64_t) s->dc_x_prev;
    int64_t fb = ((int64_t) DC_BLOCK_ALPHA * (int64_t) s->dc_y_prev) >> 15;
    int64_t acc = delta + fb;

    /* Clamp to int32 to keep state sane for the next sample */
    if (acc > INT32_MAX)
        acc = INT32_MAX;
    else if (acc < INT32_MIN)
        acc = INT32_MIN;

    int32_t dc_out = (int32_t) acc;
    s->dc_x_prev = out;
    s->dc_y_prev = dc_out;

    return soft_clip(dc_out);
}

q15_t picosynth_wave_saw(q15_t phase)
{
    return phase * 2 - Q15_MAX;
}

q15_t picosynth_wave_square(q15_t phase)
{
    return phase < Q15_MAX / 2 ? Q15_MAX : Q15_MIN;
}

q15_t picosynth_wave_triangle(q15_t phase)
{
    int32_t r = (int32_t) phase << 1;
    if (r > Q15_MAX)
        r = Q15_MAX - (r - Q15_MAX);
    return q15_sat(r * 2 - Q15_MAX);
}

q15_t picosynth_wave_falling(q15_t phase)
{
    return Q15_MAX - phase * 2;
}

q15_t picosynth_wave_exp(q15_t phase)
{
    q15_t p = Q15_MAX - phase;
    p = (p * p) >> 15;
    p = (p * p) >> 15;
    return p;
}

q15_t picosynth_wave_noise(q15_t phase)
{
    (void) phase;
    lfsr_seed ^= lfsr_seed << 13;
    lfsr_seed ^= lfsr_seed >> 17;
    lfsr_seed ^= lfsr_seed << 5;
    return (q15_t) (lfsr_seed >> 16);
}

q15_t picosynth_wave_sine(q15_t phase)
{
    return picosynth_sine_impl(phase);
}
