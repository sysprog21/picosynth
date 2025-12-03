/*
 * midiparse - Parse Standard MIDI Files and extract note events
 *
 * Usage:
 *   midiparse input.mid              # Output text format (for midi2c)
 *   midiparse input.mid -c           # Output C arrays directly
 *   midiparse input.mid -t 0         # Select specific track (default: 0)
 *   midiparse input.mid -i           # Print file info only
 *   midiparse input.mid --bpm 120    # Override tempo (BPM)
 *   midiparse input.mid --quantize 4 # Quantize to beat divisions
 *
 * Output format (text):
 *   C4 4      # C4 quarter note
 *   D#5 2     # D#5 half note
 *   - 4       # rest
 *
 * Reference: MIDI 1.0 Specification (midi.org)
 */

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "midifile.h"

#define MAX_NOTES 4096
#define MAX_CHANNELS 16

/* Note event for output */
typedef struct {
    uint32_t start_time; /* Start time in ticks */
    uint32_t end_time;   /* End time in ticks (0 if not yet ended) */
    uint8_t note;        /* MIDI note number */
    uint8_t velocity;    /* Note velocity */
    uint8_t channel;     /* MIDI channel */
} note_event_t;

/* Active note tracking (for matching note-on/note-off) */
typedef struct {
    int active[128]; /* Index into notes array, -1 if not active */
} channel_state_t;

static const char *note_names[] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B",
};

/* Convert MIDI note number to string */
static void midi_to_note_str(uint8_t midi, char *buf, size_t size)
{
    if (midi > 127) {
        snprintf(buf, size, "?");
        return;
    }
    int octave = (midi / 12) - 1;
    int note = midi % 12;
    snprintf(buf, size, "%s%d", note_names[note], octave);
}

/* Read entire file into memory */
static uint8_t *read_file(const char *path, size_t *size)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "Error: cannot open %s\n", path);
        return NULL;
    }

    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (len <= 0) {
        fprintf(stderr, "Error: empty or invalid file %s\n", path);
        fclose(fp);
        return NULL;
    }

    uint8_t *buf = malloc((size_t) len);
    if (!buf) {
        fprintf(stderr, "Error: out of memory\n");
        fclose(fp);
        return NULL;
    }

    if (fread(buf, 1, (size_t) len, fp) != (size_t) len) {
        fprintf(stderr, "Error: failed to read %s\n", path);
        free(buf);
        fclose(fp);
        return NULL;
    }

    fclose(fp);
    *size = (size_t) len;
    return buf;
}

static void print_usage(const char *prog)
{
    printf("Usage: %s [options] input.mid\n\n", prog);
    printf("Parse MIDI files and extract note events.\n\n");
    printf("Options:\n");
    printf("  -c, --c-output     Output C arrays (like midi2c)\n");
    printf(
        "  -t, --track N      Select track N (default: first track with "
        "notes)\n");
    printf("  -i, --info         Print file info only\n");
    printf("  --bpm N            Override tempo to N BPM\n");
    printf(
        "  --quantize N       Quantize to 1/N notes (4=quarter, 8=eighth)\n");
    printf("  --channel N        Filter to channel N only (0-15)\n");
    printf("  -h, --help         Show this help\n\n");
    printf("Output format (default):\n");
    printf("  NOTE BEATS    e.g., \"C4 4\" for quarter note C4\n");
    printf("  - BEATS       for rests\n");
}

static void print_file_info(const midi_file_t *mf, const char *filename)
{
    const midi_header_t *hdr = midi_file_get_header(mf);

    printf("File: %s\n", filename);
    printf("Format: %d (%s)\n", hdr->format,
           hdr->format == 0
               ? "single track"
               : (hdr->format == 1 ? "multi-track sync" : "multi-track async"));
    printf("Tracks: %d\n", hdr->ntracks);

    if (hdr->uses_smpte) {
        printf("Timing: SMPTE %d fps, %d ticks/frame\n", hdr->smpte_fps,
               hdr->smpte_res);
    } else {
        printf("Timing: %d ticks per quarter note\n", hdr->division);
    }

    printf("Default tempo: %.1f BPM\n", 60000000.0 / 500000.0);
}

/* Compare function for sorting notes by start time */
static int compare_notes(const void *a, const void *b)
{
    const note_event_t *na = (const note_event_t *) a;
    const note_event_t *nb = (const note_event_t *) b;

    if (na->start_time != nb->start_time)
        return (int) (na->start_time - nb->start_time);
    return (int) na->note - (int) nb->note;
}

int main(int argc, char **argv)
{
    const char *input_file = NULL;
    int c_output = 0;
    int info_only = 0;
    int track_num = -1; /* -1 = auto-select first track with notes */
    int quantize = 0;
    int filter_channel = -1;

    (void) 0; /* Reserved for future --bpm override */

    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "-c") == 0 ||
                   strcmp(argv[i], "--c-output") == 0) {
            c_output = 1;
        } else if (strcmp(argv[i], "-i") == 0 ||
                   strcmp(argv[i], "--info") == 0) {
            info_only = 1;
        } else if (strcmp(argv[i], "-t") == 0 ||
                   strcmp(argv[i], "--track") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: -t requires track number\n");
                return 1;
            }
            track_num = atoi(argv[i]);
        } else if (strcmp(argv[i], "--bpm") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: --bpm requires value\n");
                return 1;
            }
            /* TODO: implement tempo override */
            (void) atoi(argv[i]);
        } else if (strcmp(argv[i], "--quantize") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: --quantize requires value\n");
                return 1;
            }
            quantize = atoi(argv[i]);
        } else if (strcmp(argv[i], "--channel") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: --channel requires value (0-15)\n");
                return 1;
            }
            filter_channel = atoi(argv[i]);
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "Error: unknown option %s\n", argv[i]);
            return 1;
        } else {
            input_file = argv[i];
        }
    }

    if (!input_file) {
        fprintf(stderr, "Error: no input file specified\n");
        print_usage(argv[0]);
        return 1;
    }

    /* Read MIDI file */
    size_t file_size;
    uint8_t *file_data = read_file(input_file, &file_size);
    if (!file_data)
        return 1;

    /* Parse MIDI file */
    midi_file_t mf;
    midi_error_t err = midi_file_open(&mf, file_data, file_size);
    if (err != MIDI_OK) {
        const char *err_msg;
        switch (err) {
        case MIDI_ERR_INVALID_HEADER:
            err_msg = "not a valid MIDI file";
            break;
        case MIDI_ERR_UNSUPPORTED_FMT:
            err_msg = "unsupported MIDI format (only format 0/1 supported)";
            break;
        case MIDI_ERR_TRUNCATED:
            err_msg = "file truncated";
            break;
        default:
            err_msg = "parse error";
            break;
        }
        fprintf(stderr, "Error: %s: %s\n", input_file, err_msg);
        free(file_data);
        return 1;
    }

    const midi_header_t *hdr = midi_file_get_header(&mf);

    if (info_only) {
        print_file_info(&mf, input_file);

        /* Scan each track for info */
        for (int t = 0; t < hdr->ntracks; t++) {
            if (midi_file_select_track(&mf, (uint16_t) t) != MIDI_OK)
                continue;

            midi_event_t evt;
            int note_count = 0;
            uint32_t duration = 0;
            char track_name[256] = "";

            while (midi_file_next_event(&mf, &evt) == MIDI_OK) {
                duration = evt.abs_time;
                if (midi_is_note_on(&evt))
                    note_count++;
                if (evt.type == 0xFF && evt.meta_type == MIDI_META_TRACK_NAME &&
                    evt.meta_length < sizeof(track_name)) {
                    memcpy(track_name, evt.meta_data, evt.meta_length);
                    track_name[evt.meta_length] = '\0';
                }
            }

            printf("\nTrack %d: %s\n", t,
                   track_name[0] ? track_name : "(unnamed)");
            printf("  Notes: %d\n", note_count);
            printf("  Duration: %u ticks (%u ms)\n", duration,
                   midi_ticks_to_ms(&mf, duration));
        }

        free(file_data);
        return 0;
    }

    /* Collect notes from tracks */
    note_event_t *notes = malloc(MAX_NOTES * sizeof(note_event_t));
    if (!notes) {
        fprintf(stderr, "Error: out of memory\n");
        free(file_data);
        return 1;
    }
    int note_count = 0;

    channel_state_t channels[MAX_CHANNELS];
    memset(channels, 0xFF, sizeof(channels)); /* -1 = not active */

    /* Process tracks */
    int start_track = (track_num >= 0) ? track_num : 0;
    int end_track = (track_num >= 0) ? track_num + 1 : hdr->ntracks;

    for (int t = start_track; t < end_track && t < hdr->ntracks; t++) {
        if (midi_file_select_track(&mf, (uint16_t) t) != MIDI_OK)
            continue;

        midi_event_t evt;
        while (midi_file_next_event(&mf, &evt) == MIDI_OK) {
            if (filter_channel >= 0 && evt.channel != filter_channel)
                continue;

            if (midi_is_note_on(&evt)) {
                /* Note on */
                if (note_count >= MAX_NOTES) {
                    fprintf(stderr, "Warning: too many notes (max %d)\n",
                            MAX_NOTES);
                    break;
                }

                /* End any existing note on same pitch/channel */
                int prev_idx = channels[evt.channel].active[evt.data1];
                if (prev_idx >= 0 && notes[prev_idx].end_time == 0) {
                    notes[prev_idx].end_time = evt.abs_time;
                }

                notes[note_count].start_time = evt.abs_time;
                notes[note_count].end_time = 0;
                notes[note_count].note = evt.data1;
                notes[note_count].velocity = evt.data2;
                notes[note_count].channel = evt.channel;
                channels[evt.channel].active[evt.data1] = note_count;
                note_count++;
            } else if (midi_is_note_off(&evt)) {
                /* Note off */
                int idx = channels[evt.channel].active[evt.data1];
                if (idx >= 0 && notes[idx].end_time == 0) {
                    notes[idx].end_time = evt.abs_time;
                    channels[evt.channel].active[evt.data1] = -1;
                }
            }
        }
    }

    if (note_count == 0) {
        fprintf(stderr, "Error: no notes found\n");
        free(notes);
        free(file_data);
        return 1;
    }

    /* Sort notes by start time */
    qsort(notes, (size_t) note_count, sizeof(note_event_t), compare_notes);

    /* End any unclosed notes at the last known time */
    uint32_t last_time = notes[note_count - 1].start_time;
    for (int i = 0; i < note_count; i++) {
        if (notes[i].end_time == 0)
            notes[i].end_time = last_time + hdr->division;
    }

    /* Calculate beat duration in ticks */
    uint32_t ticks_per_beat = hdr->division;
    if (quantize > 0) {
        ticks_per_beat = hdr->division * 4 / (uint32_t) quantize;
    }

    /* Generate output */
    if (c_output) {
        /* C array output */
        printf("#ifndef __MELODY_H\n");
        printf("#define __MELODY_H\n\n");
        printf("/* Generated by midiparse from %s */\n", input_file);
        printf("/* %d notes */\n\n", note_count);

        printf("const uint8_t melody[] = {\n");
        uint32_t prev_end = 0;

        for (int i = 0; i < note_count; i++) {
            /* Insert rest if there's a gap */
            if (notes[i].start_time > prev_end) {
                uint32_t gap = notes[i].start_time - prev_end;
                int rest_beats =
                    (int) ((gap + ticks_per_beat / 2) / ticks_per_beat);
                if (rest_beats > 0) {
                    printf("    0, %d, /* rest */\n", rest_beats);
                }
            }

            uint32_t duration = notes[i].end_time - notes[i].start_time;
            int beats =
                (int) ((duration + ticks_per_beat / 2) / ticks_per_beat);
            if (beats < 1)
                beats = 1;

            char note_str[8];
            midi_to_note_str(notes[i].note, note_str, sizeof(note_str));
            printf("    %d, %d, /* %s */\n", notes[i].note, beats, note_str);

            prev_end = notes[i].end_time;
        }

        printf("};\n\n");
        printf("#define MELODY_LENGTH %d\n\n", note_count);
        printf("#endif /* __MELODY_H */\n");
    } else {
        /* Text format output (for midi2c) */
        printf("# Generated by midiparse from %s\n", input_file);
        printf("# %d notes, %d ticks/quarter\n\n", note_count, hdr->division);

        uint32_t prev_end = 0;

        for (int i = 0; i < note_count; i++) {
            /* Insert rest if there's a gap */
            if (notes[i].start_time > prev_end) {
                uint32_t gap = notes[i].start_time - prev_end;
                int rest_beats =
                    (int) ((gap + ticks_per_beat / 2) / ticks_per_beat);
                if (rest_beats > 0) {
                    printf("- %d\n", rest_beats);
                }
            }

            uint32_t duration = notes[i].end_time - notes[i].start_time;
            int beats =
                (int) ((duration + ticks_per_beat / 2) / ticks_per_beat);
            if (beats < 1)
                beats = 1;

            char note_str[8];
            midi_to_note_str(notes[i].note, note_str, sizeof(note_str));
            printf("%s %d\n", note_str, beats);

            prev_end = notes[i].end_time;
        }
    }

    free(notes);
    free(file_data);
    return 0;
}
