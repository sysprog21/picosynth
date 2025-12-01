#include <stdio.h>
#include <stdlib.h>

#include "melody.h"
#include "picosynth.h"

/* Detune for inharmonicity (piano strings are slightly sharp on overtones) */
static q15_t octave_detune;

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
    picosynth_t *picosynth = picosynth_create(2, 8);
    if (!picosynth) {
        printf("Failed to create synth\n");
        return 1;
    }

    /* Voice 0: Piano main tone
     * - Node 0: Output filter (LP for warmth)
     * - Node 1: Main envelope (ADSR)
     * - Node 2: Fundamental oscillator (sine)
     * - Node 3: 2nd partial oscillator (triangle)
     * - Node 4: Mixer combining partials
     */
    picosynth_voice_t *v = picosynth_get_voice(picosynth, 0);
    picosynth_node_t *v0_flt = picosynth_voice_get_node(v, 0);
    picosynth_node_t *v0_env = picosynth_voice_get_node(v, 1);
    picosynth_node_t *v0_osc1 = picosynth_voice_get_node(v, 2);
    picosynth_node_t *v0_osc2 = picosynth_voice_get_node(v, 3);
    picosynth_node_t *v0_mix = picosynth_voice_get_node(v, 4);

    /* Main envelope: instant attack, piano-like decay */
    picosynth_init_env(v0_env, NULL, 12000,        /* attack - instant */
                       350,                        /* decay */
                       (q15_t) (Q15_MAX * 2 / 10), /* sustain (20%) */
                       50);                        /* release */

    /* Fundamental: sine wave at note pitch */
    picosynth_init_osc(v0_osc1, &v0_env->out, picosynth_voice_freq_ptr(v),
                       picosynth_wave_sine);

    /* 2nd partial: triangle wave, slightly detuned for richness */
    picosynth_init_osc(v0_osc2, &v0_env->out, picosynth_voice_freq_ptr(v),
                       picosynth_wave_triangle);
    v0_osc2->osc.detune = &octave_detune;

    /* Mix fundamental + 2nd partial */
    picosynth_init_mix(v0_mix, NULL, &v0_osc1->out, &v0_osc2->out, NULL);

    /* Output LP filter for warmth */
    picosynth_init_lp(v0_flt, NULL, &v0_mix->out, 5000);

    /* Set output node and compute usage mask */
    picosynth_voice_set_out(v, 0);

    /*
     * Voice 1: Piano attack transient + upper harmonics
     * - Node 0: Output filter (LP to tame harshness)
     * - Node 1: Fast decay envelope (hammer transient)
     * - Node 2: Bright oscillator (sawtooth for harmonics)
     * - Node 3: Second envelope for body
     * - Node 4: Body oscillator (triangle)
     * - Node 5: Mixer
     */
    v = picosynth_get_voice(picosynth, 1);
    picosynth_node_t *v1_flt = picosynth_voice_get_node(v, 0);
    picosynth_node_t *v1_env1 = picosynth_voice_get_node(v, 1);
    picosynth_node_t *v1_osc1 = picosynth_voice_get_node(v, 2);
    picosynth_node_t *v1_env2 = picosynth_voice_get_node(v, 3);
    picosynth_node_t *v1_osc2 = picosynth_voice_get_node(v, 4);
    picosynth_node_t *v1_mix = picosynth_voice_get_node(v, 5);

    /* Transient envelope: very fast attack and decay (hammer strike) */
    picosynth_init_env(v1_env1, NULL, 15000,   /* attack - very fast */
                       1200,                   /* decay - fast */
                       (q15_t) (Q15_MAX / 25), /* sustain (4%) */
                       30);                    /* release - quick */

    /* Bright transient: sawtooth for rich harmonics */
    picosynth_init_osc(v1_osc1, &v1_env1->out, picosynth_voice_freq_ptr(v),
                       picosynth_wave_saw);

    /* Body envelope: slower decay */
    picosynth_init_env(v1_env2, NULL, 10000,         /* attack */
                       250,                          /* decay - slower */
                       (q15_t) (Q15_MAX * 15 / 100), /* sustain (15%) */
                       40);                          /* release */

    /* Body tone: triangle for softer harmonics */
    picosynth_init_osc(v1_osc2, &v1_env2->out, picosynth_voice_freq_ptr(v),
                       picosynth_wave_triangle);

    /* Mix transient + body */
    picosynth_init_mix(v1_mix, NULL, &v1_osc1->out, &v1_osc2->out, NULL);

    /* Output LP filter */
    picosynth_init_lp(v1_flt, NULL, &v1_mix->out, 6500);

    /* Set output node and compute usage mask */
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
                picosynth_note_on(picosynth, 0, note);
                picosynth_note_on(picosynth, 1, note);
                /* Slight detune for richness (~0.2% sharp) */
                octave_detune = *picosynth_voice_freq_ptr(
                                    picosynth_get_voice(picosynth, 0)) /
                                500;
            }
            note_idx++;
            if (note_idx >= sizeof(melody))
                break;
        } else if (note_dur < 200) {
            picosynth_note_off(picosynth, 0);
            picosynth_note_off(picosynth, 1);
        }
        note_dur--;

        audio[samples++] = picosynth_process(picosynth);
    }

    int res = write_wav("output.wav", audio, samples);
    free(audio);
    picosynth_destroy(picosynth);
    return res;
}
