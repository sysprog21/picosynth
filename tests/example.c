#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "melody.h"
#include "picosynth.h"

/* Frequency offsets for true partials (added to base freq via detune) */
static q15_t partial2_offset; /* 2nd partial: base_freq (so total = 2*base) */
static q15_t partial3_offset; /* 3rd partial: 2*base_freq (so total = 3*base) */

/* Filter node pointers for dynamic frequency tracking */
static picosynth_node_t *g_flt_main;
static picosynth_node_t *g_flt_harm;
static picosynth_node_t *g_flt_noise;

/* Inharmonicity coefficient table (Q15 format).
 * B scales with frequency squared: B ≈ 7e-5 * (f/440)^2
 * Real piano: Bass ~0.00005, Middle ~0.0001, Treble ~0.001+
 */
static q15_t get_inharmonicity_coeff(uint8_t note)
{
    /* Pre-calculated B values for key MIDI notes (Q15 scaled by 32768)
     * Using formula: B = 7e-5 * (f/440)^2, clamped 2e-5 to 2e-3
     */
    static const q15_t B_table[12] = {
        /* C    C#   D    D#   E    F    F#   G    G#   A    A#   B  */
        1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 3, 3, /* Base values, scaled by octave */
    };

    int octave = note / 12;
    int semitone = note % 12;

    /* Scale B by octave (doubles each octave due to f^2 relationship) */
    int32_t B = B_table[semitone];
    for (int i = 0; i < octave - 4;
         i++)    /* Octave 4 (MIDI 48-59) is reference */
        B <<= 2; /* B quadruples per octave (f^2) */
    for (int i = octave; i < 4; i++)
        B >>= 2;

    /* Clamp to realistic range */
    if (B < 1)
        B = 1;
    if (B > 65)
        B = 65; /* ~0.002 in Q15 */

    return (q15_t) B;
}

/* Calculate partial frequencies with inharmonicity stretching.
 * Real piano formula: fn = n * f1 * sqrt(1 + B * n^2)
 * Approximation: fn ≈ n * f1 * (1 + B * n^2 / 2) for small B
 *
 * Sets global variables for oscillator detune offsets.
 */
static void calc_partial_frequencies(uint8_t note, q15_t base_freq)
{
    q15_t B = get_inharmonicity_coeff(note);

    /* 2nd partial: freq = 2*f1*(1 + B*4/2) = 2*f1 + 4*B*f1
     * offset = f1 + 4*B*f1 (since detune is added to base_freq)
     */
    int32_t stretch2 = ((int32_t) B * 4 * base_freq) >> 15;
    partial2_offset = q15_sat(base_freq + stretch2);

    /* 3rd partial: freq = 3*f1*(1 + B*9/2) ≈ 3*f1 + 13.5*B*f1
     * offset = 2*f1 + inharmonic stretch
     * Only include if 3*f1 < Nyquist (5512 Hz at 11025 sample rate)
     */
    int32_t stretch3 = ((int32_t) B * 14 * base_freq) >> 15;
    int32_t offset3 = 2 * (int32_t) base_freq + stretch3;
    /* Clamp to Q15 range - high notes may overflow */
    partial3_offset = q15_sat(offset3);
}

/* Frequency-tracked SVF filter coefficient.
 * Conservative cutoffs for warm piano sound.
 * fc = 600 + 20 * (note - 48), clamped 500-1500 Hz
 */
static q15_t calc_svf_freq(uint8_t note)
{
    /* Conservative frequency tracking for warm sound */
    int32_t fc = 600 + 20 * ((int32_t) note - 48);
    if (fc < 500)
        fc = 500; /* Bass: warm */
    if (fc > 1500)
        fc = 1500; /* Treble: still warm, not bright */

    return picosynth_svf_freq((uint16_t) fc);
}

static int write_wav(char *filename, int16_t *buf, uint32_t samples)
{
    FILE *f = fopen(filename, "wb");
    if (!f) {
        printf("Failed to open output file\n");
        return 1;
    }

    uint32_t sample_rate = SAMPLE_RATE;
    uint32_t file_size = samples * 2 + 36;
    uint32_t byte_rate = SAMPLE_RATE * 2;
    uint32_t block_align = 2;
    uint32_t bits_per_sample = 16;
    uint32_t data_size = samples * 2;
    uint32_t fmt_size = 16;
    uint16_t format = 1;
    uint16_t channels = 1;

    fwrite("RIFF", 1, 4, f);
    fwrite(&file_size, 4, 1, f);
    fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);
    fwrite(&fmt_size, 4, 1, f);
    fwrite(&format, 2, 1, f);
    fwrite(&channels, 2, 1, f);
    fwrite(&sample_rate, 4, 1, f);
    fwrite(&byte_rate, 4, 1, f);
    fwrite(&block_align, 2, 1, f);
    fwrite(&bits_per_sample, 2, 1, f);
    fwrite("data", 1, 4, f);
    fwrite(&data_size, 4, 1, f);
    fwrite(buf, 2, samples, f);
    fclose(f);
    return 0;
}

int main(void)
{
    picosynth_t *picosynth = picosynth_create(4, 8);
    if (!picosynth) {
        printf("Failed to create synth\n");
        return 1;
    }

    q15_t piano_q = Q15_MAX; /* Max damping, no resonance */

    /*
     * PER-PARTIAL DECAY ARCHITECTURE:
     * Voice 0: Fundamental (1st partial) - SLOWEST decay, sustained body
     * Voice 1: 2nd-3rd partials - MEDIUM decay
     * Voice 2: Upper partials (4th+) - FAST decay, initial brightness
     * Voice 3: Hammer noise - VERY FAST decay, percussive attack
     */

    /* Voice 0: FUNDAMENTAL (1st partial) - Slowest decay
     * Sustained "body" of the piano tone.
     */
    picosynth_voice_t *v = picosynth_get_voice(picosynth, 0);
    picosynth_node_t *v0_flt = picosynth_voice_get_node(v, 0);
    picosynth_node_t *v0_env = picosynth_voice_get_node(v, 1);
    picosynth_node_t *v0_osc = picosynth_voice_get_node(v, 2);

    /* Fundamental envelope: VERY slow decay for sustained body
     * Real piano fundamental sustains for seconds
     */
    picosynth_init_env(v0_env, NULL,
                       &(picosynth_env_params_t) {
                           .attack = 10000, /* smooth onset */
                           .hold = 0,       /* none (immediate decay) */
                           .decay = 60,     /* VERY slow (sustains long) */
                           .sustain = (q15_t) (Q15_MAX * 15 / 100), /* 15% */
                           .release = 40, /* gentle fade */
                       });

    /* Pure sine at fundamental frequency */
    picosynth_init_osc(v0_osc, &v0_env->out, picosynth_voice_freq_ptr(v),
                       picosynth_wave_sine);

    /* Filter: moderate cutoff, lets fundamental through cleanly */
    picosynth_init_svf_lp(v0_flt, NULL, &v0_osc->out, picosynth_svf_freq(1200),
                          piano_q);
    g_flt_main = v0_flt;

    picosynth_voice_set_out(v, 0);

    /* Voice 1: 2nd-3rd PARTIALS - Medium decay
     * Defines "warmth", fades at medium rate.
     */
    v = picosynth_get_voice(picosynth, 1);
    picosynth_node_t *v1_flt = picosynth_voice_get_node(v, 0);
    picosynth_node_t *v1_env1 = picosynth_voice_get_node(v, 1);
    picosynth_node_t *v1_osc1 = picosynth_voice_get_node(v, 2);
    picosynth_node_t *v1_env2 = picosynth_voice_get_node(v, 3);
    picosynth_node_t *v1_osc2 = picosynth_voice_get_node(v, 4);
    picosynth_node_t *v1_mix = picosynth_voice_get_node(v, 5);

    /* 2nd partial envelope: medium decay (faster than fundamental) */
    picosynth_init_env(v1_env1, NULL,
                       &(picosynth_env_params_t) {
                           .attack = 8000,
                           .hold = 0,
                           .decay = 150, /* MEDIUM rate */
                           .sustain = (q15_t) (Q15_MAX * 8 / 100), /* 8% */
                           .release = 50,
                       });

    /* 2nd partial: sine at 2*base_freq with inharmonicity stretch */
    picosynth_init_osc(v1_osc1, &v1_env1->out, picosynth_voice_freq_ptr(v),
                       picosynth_wave_sine);
    v1_osc1->osc.detune = &partial2_offset; /* offset = f1, so total = 2*f1 */

    /* 3rd partial envelope: slightly faster decay than 2nd */
    picosynth_init_env(v1_env2, NULL,
                       &(picosynth_env_params_t) {
                           .attack = 7000,
                           .hold = 0,
                           .decay = 300, /* faster than 2nd partial */
                           .sustain = (q15_t) (Q15_MAX * 4 / 100), /* 4% */
                           .release = 40,
                       });

    /* 3rd partial: sine at 3*base_freq with inharmonicity stretch */
    picosynth_init_osc(v1_osc2, &v1_env2->out, picosynth_voice_freq_ptr(v),
                       picosynth_wave_sine);
    v1_osc2->osc.detune = &partial3_offset; /* offset = 2*f1, so total = 3*f1 */

    picosynth_init_mix(v1_mix, NULL, &v1_osc1->out, &v1_osc2->out, NULL);

    /* Filter: moderate cutoff for warm partials */
    picosynth_init_svf_lp(v1_flt, NULL, &v1_mix->out, picosynth_svf_freq(1200),
                          piano_q);
    g_flt_harm = v1_flt;

    picosynth_voice_set_out(v, 0);

    /* Voice 2: UPPER PARTIALS (4th+) - Fast decay
     * Subtle brightness that fades quickly.
     */
    v = picosynth_get_voice(picosynth, 2);
    picosynth_node_t *v2_flt = picosynth_voice_get_node(v, 0);
    picosynth_node_t *v2_env = picosynth_voice_get_node(v, 1);
    picosynth_node_t *v2_osc = picosynth_voice_get_node(v, 2);

    /* Upper partials envelope: FAST decay, LOW level
     * Subtle contribution that fades quickly
     */
    picosynth_init_env(v2_env, NULL,
                       &(picosynth_env_params_t) {
                           .attack = 5000, /* soft onset */
                           .hold = 0,
                           .decay = 800, /* fast fade */
                           .sustain = (q15_t) (Q15_MAX * 1 / 100), /* 1% */
                           .release = 20,                          /* quick */
                       });

    /* Sine for clean sound - no harsh harmonics */
    picosynth_init_osc(v2_osc, &v2_env->out, picosynth_voice_freq_ptr(v),
                       picosynth_wave_sine);

    /* Moderate filter cutoff */
    picosynth_init_svf_lp(v2_flt, NULL, &v2_osc->out, picosynth_svf_freq(1500),
                          piano_q);

    picosynth_voice_set_out(v, 0);

    /* Voice 3: HAMMER NOISE - Very subtle, very fast decay
     * Minimal noise for slight texture, narrow band.
     */
    v = picosynth_get_voice(picosynth, 3);
    picosynth_node_t *v3_lp = picosynth_voice_get_node(v, 0);
    picosynth_node_t *v3_env = picosynth_voice_get_node(v, 1);
    picosynth_node_t *v3_noise = picosynth_voice_get_node(v, 2);
    picosynth_node_t *v3_hp = picosynth_voice_get_node(v, 3);

    /* Hammer noise: very subtle, almost imperceptible
     * Just adds slight "thump" texture, not harsh attack
     */
    picosynth_init_env(v3_env, NULL,
                       &(picosynth_env_params_t) {
                           .attack = 8000, /* soft */
                           .hold = 0,
                           .decay = 6000, /* very fast */
                           .sustain = 0,  /* none */
                           .release = 50,
                       });

    /* White noise source */
    picosynth_init_osc(v3_noise, &v3_env->out, picosynth_voice_freq_ptr(v),
                       picosynth_wave_noise);

    /* Narrow bandpass: 200-800 Hz for subtle low-mid "thump" only */
    picosynth_init_svf_hp(v3_hp, NULL, &v3_noise->out, picosynth_svf_freq(200),
                          piano_q);
    picosynth_init_svf_lp(v3_lp, NULL, &v3_hp->out, picosynth_svf_freq(800),
                          piano_q);
    g_flt_noise = v3_lp;

    picosynth_voice_set_out(v, 0);

    /* Allocate audio buffer (60 seconds max) */
    int16_t *audio = malloc(sizeof(int16_t) * SAMPLE_RATE * 60);
    if (!audio) {
        printf("Failed to allocate audio buffer\n");
        picosynth_destroy(picosynth);
        return 1;
    }
    uint32_t samples = 0;

    /* Play melody from melody.h */
    uint32_t note_dur = 0;
    uint32_t note_idx = 0;
    for (;;) {
        if (note_dur == 0) {
            note_dur = PICOSYNTH_MS(2000 / melody_beats[note_idx]);
            uint8_t note = melody[note_idx];
            if (note) {
                /* Trigger all 4 voices for per-partial decay */
                picosynth_note_on(picosynth, 0, note); /* Fundamental */
                picosynth_note_on(picosynth, 1, note); /* 2nd-3rd partials */
                picosynth_note_on(picosynth, 2, note); /* Upper partials */
                picosynth_note_on(picosynth, 3, note); /* Hammer noise */

                /* Calculate true partial frequencies with inharmonicity */
                q15_t base_freq = *picosynth_voice_freq_ptr(
                    picosynth_get_voice(picosynth, 0));
                calc_partial_frequencies(note, base_freq);

                /* Frequency-tracked filter: fundamental voice */
                q15_t svf_f = calc_svf_freq(note);
                picosynth_svf_set_freq(g_flt_main, svf_f);

                /* 2nd-3rd partials: moderate cutoff for warmth */
                int32_t fc_harm = 700 + 15 * ((int32_t) note - 48);
                if (fc_harm < 500)
                    fc_harm = 500;
                if (fc_harm > 1400)
                    fc_harm = 1400;
                picosynth_svf_set_freq(g_flt_harm,
                                       picosynth_svf_freq((uint16_t) fc_harm));

                /* Noise filter: low cutoff for subtle thump */
                int32_t fc_noise = 500 + 10 * ((int32_t) note - 48);
                if (fc_noise < 400)
                    fc_noise = 400;
                if (fc_noise > 1000)
                    fc_noise = 1000;
                picosynth_svf_set_freq(g_flt_noise,
                                       picosynth_svf_freq((uint16_t) fc_noise));
            }
            note_idx++;
            if (note_idx >= sizeof(melody))
                break;
        } else if (note_dur < 200) {
            /* Release all 4 voices */
            picosynth_note_off(picosynth, 0);
            picosynth_note_off(picosynth, 1);
            picosynth_note_off(picosynth, 2);
            picosynth_note_off(picosynth, 3);
        }
        note_dur--;

        audio[samples++] = picosynth_process(picosynth);
    }

    int res = write_wav("output.wav", audio, samples);
    free(audio);
    picosynth_destroy(picosynth);
    return res;
}
