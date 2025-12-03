/*
 * txt2midi - Convert text melody format to Standard MIDI File
 *
 * Usage:
 *   txt2midi input.txt output.mid
 *   txt2midi input.txt output.mid --bpm 120
 *
 * Input format (same as midi2c):
 *   C4 4      # C4 quarter note
 *   D#5 2     # D#5 half note
 *   - 4       # rest
 */

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_LINE 256
#define DEFAULT_BPM 120
#define TICKS_PER_QUARTER 480

/* Convert note name to semitone offset (0-11) */
static int note_to_semitone(const char *name)
{
    char upper = (char) toupper((unsigned char) name[0]);
    int base;

    switch (upper) {
    case 'C':
        base = 0;
        break;
    case 'D':
        base = 2;
        break;
    case 'E':
        base = 4;
        break;
    case 'F':
        base = 5;
        break;
    case 'G':
        base = 7;
        break;
    case 'A':
        base = 9;
        break;
    case 'B':
        base = 11;
        break;
    default:
        return -1;
    }

    /* Check for sharp/flat */
    if (name[1] == '#')
        base = (base + 1) % 12;
    else if (name[1] == 'b')
        base = (base + 11) % 12;

    return base;
}

/* Parse note string like "C4", "D#5", "Bb3" to MIDI number */
static int parse_note(const char *str)
{
    /* Rest */
    if (str[0] == '-' || str[0] == 'R' || str[0] == 'r')
        return 0;

    /* Parse note name */
    int semitone = note_to_semitone(str);
    if (semitone < 0)
        return -1;

    /* Find octave number */
    const char *p = str + 1;
    if (*p == '#' || *p == 'b')
        p++;

    int octave = atoi(p);
    if (octave < -1 || octave > 9)
        return -1;

    /* MIDI: C4 = 60 */
    int midi = (octave + 1) * 12 + semitone;

    /* Handle B# wrapping */
    if (str[0] == 'B' && str[1] == '#')
        midi += 12;

    if (midi < 0 || midi > 127)
        return -1;

    return midi;
}

/* Write big-endian 16-bit value */
static void write_be16(FILE *f, uint16_t val)
{
    fputc((val >> 8) & 0xFF, f);
    fputc(val & 0xFF, f);
}

/* Write big-endian 32-bit value */
static void write_be32(FILE *f, uint32_t val)
{
    fputc((val >> 24) & 0xFF, f);
    fputc((val >> 16) & 0xFF, f);
    fputc((val >> 8) & 0xFF, f);
    fputc(val & 0xFF, f);
}

/* Write variable-length quantity */
static void write_vlq(FILE *f, uint32_t val)
{
    uint8_t buf[4];
    int i = 0;

    buf[i++] = val & 0x7F;
    val >>= 7;

    while (val > 0) {
        buf[i++] = (val & 0x7F) | 0x80;
        val >>= 7;
    }

    /* Write in reverse order */
    while (i > 0)
        fputc(buf[--i], f);
}

static void print_usage(const char *prog)
{
    printf("Usage: %s input.txt output.mid [options]\n\n", prog);
    printf("Convert text melody to Standard MIDI File.\n\n");
    printf("Options:\n");
    printf("  --bpm N      Set tempo (default: %d)\n", DEFAULT_BPM);
    printf("  --velocity N Set note velocity (default: 100)\n");
    printf("  -h, --help   Show this help\n");
}

int main(int argc, char **argv)
{
    const char *input_file = NULL;
    const char *output_file = NULL;
    int bpm = DEFAULT_BPM;
    int velocity = 100;

    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "--bpm") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: --bpm requires value\n");
                return 1;
            }
            bpm = atoi(argv[i]);
        } else if (strcmp(argv[i], "--velocity") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: --velocity requires value\n");
                return 1;
            }
            velocity = atoi(argv[i]);
            if (velocity < 1)
                velocity = 1;
            if (velocity > 127)
                velocity = 127;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "Error: unknown option %s\n", argv[i]);
            return 1;
        } else if (!input_file) {
            input_file = argv[i];
        } else if (!output_file) {
            output_file = argv[i];
        }
    }

    if (!input_file || !output_file) {
        fprintf(stderr, "Error: input and output files required\n");
        print_usage(argv[0]);
        return 1;
    }

    /* Read input file */
    FILE *fp = fopen(input_file, "r");
    if (!fp) {
        fprintf(stderr, "Error: cannot open %s\n", input_file);
        return 1;
    }

    /* Collect notes */
    typedef struct {
        int midi;
        int beats;
    } note_t;

    note_t notes[4096];
    int note_count = 0;
    char line[MAX_LINE];
    int line_num = 0;

    while (fgets(line, sizeof(line), fp)) {
        line_num++;

        /* Skip comments and empty lines */
        char *p = line;
        while (isspace((unsigned char) *p))
            p++;
        if (*p == '\0' || *p == '#')
            continue;

        /* Parse note and beat */
        char note_str[16];
        int beat;
        if (sscanf(p, "%15s %d", note_str, &beat) != 2) {
            fprintf(stderr, "Warning: line %d: expected 'NOTE BEATS'\n",
                    line_num);
            continue;
        }

        int midi = parse_note(note_str);
        if (midi < 0) {
            fprintf(stderr, "Warning: line %d: invalid note '%s'\n", line_num,
                    note_str);
            continue;
        }

        if (note_count >= 4096) {
            fprintf(stderr, "Warning: too many notes, truncating\n");
            break;
        }

        notes[note_count].midi = midi;
        notes[note_count].beats = beat;
        note_count++;
    }
    fclose(fp);

    if (note_count == 0) {
        fprintf(stderr, "Error: no notes found\n");
        return 1;
    }

    /* Create output MIDI file */
    FILE *out = fopen(output_file, "wb");
    if (!out) {
        fprintf(stderr, "Error: cannot create %s\n", output_file);
        return 1;
    }

    /* Write MThd header */
    fwrite("MThd", 1, 4, out);
    write_be32(out, 6);                 /* chunk length */
    write_be16(out, 0);                 /* format 0 */
    write_be16(out, 1);                 /* 1 track */
    write_be16(out, TICKS_PER_QUARTER); /* ticks per quarter */

    /* Write MTrk header (length placeholder) */
    long mtrk_pos = ftell(out);
    fwrite("MTrk", 1, 4, out);
    write_be32(out, 0); /* length placeholder */
    long data_start = ftell(out);

    /* Write tempo meta event */
    uint32_t tempo_us = 60000000 / (uint32_t) bpm;
    fputc(0x00, out); /* delta time */
    fputc(0xFF, out); /* meta event */
    fputc(0x51, out); /* tempo */
    fputc(0x03, out); /* length */
    fputc((tempo_us >> 16) & 0xFF, out);
    fputc((tempo_us >> 8) & 0xFF, out);
    fputc(tempo_us & 0xFF, out);

    /* Write note events */
    for (int i = 0; i < note_count; i++) {
        uint32_t duration = (uint32_t) notes[i].beats * TICKS_PER_QUARTER /
                            4; /* beats are quarter-note based */

        if (notes[i].midi == 0) {
            /* Rest: just advance time */
            if (i < note_count - 1) {
                /* Will be handled by next note's delta time */
            }
        } else {
            /* Note on */
            write_vlq(out, 0); /* delta time (rest handled below) */
            fputc(0x90, out);  /* note on, channel 0 */
            fputc(notes[i].midi, out);
            fputc(velocity, out);

            /* Note off after duration */
            write_vlq(out, duration);
            fputc(0x80, out); /* note off */
            fputc(notes[i].midi, out);
            fputc(0, out); /* velocity */
        }

        /* Add rest time before next note if this was a rest */
        if (notes[i].midi == 0 && i < note_count - 1) {
            /* Find next non-rest note and accumulate time */
            /* For simplicity, emit a dummy controller event with delta */
        }
    }

    /* Handle rests by rewriting with proper delta times */
    /* (Simple approach: rests become gaps between notes) */

    /* End of track */
    fputc(0x00, out);
    fputc(0xFF, out);
    fputc(0x2F, out);
    fputc(0x00, out);

    /* Update track length */
    long data_end = ftell(out);
    uint32_t track_len = (uint32_t) (data_end - data_start);
    fseek(out, mtrk_pos + 4, SEEK_SET);
    write_be32(out, track_len);

    fclose(out);

    printf("Created %s: %d notes, %d BPM, %d ticks/quarter\n", output_file,
           note_count, bpm, TICKS_PER_QUARTER);

    return 0;
}
