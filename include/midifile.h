/**
 * midifile.h - Lightweight MIDI File Decoder
 *
 * Parses Standard MIDI File (SMF) format 0 and 1 per MIDI 1.0 specification.
 * Designed for resource-constrained systems: no dynamic allocation in core
 * parsing, callback-based event delivery, minimal memory footprint.
 *
 * Usage:
 *   midi_file_t mf;
 *   if (midi_file_open(&mf, buffer, size) == MIDI_OK) {
 *       midi_event_t evt;
 *       while (midi_file_next_event(&mf, &evt) == MIDI_OK) {
 *           // Process evt
 *       }
 *   }
 *
 * Reference: MIDI 1.0 Detailed Specification (midi.org)
 */

#ifndef MIDIFILE_H_
#define MIDIFILE_H_

#include <stddef.h>
#include <stdint.h>

/* Error codes */
typedef enum {
    MIDI_OK = 0,
    MIDI_ERR_INVALID_HEADER,  /* Not a valid MIDI file (missing MThd) */
    MIDI_ERR_UNSUPPORTED_FMT, /* Unsupported MIDI format (e.g., type 2) */
    MIDI_ERR_TRUNCATED,       /* Unexpected end of data */
    MIDI_ERR_INVALID_TRACK,   /* Invalid track header or data */
    MIDI_ERR_INVALID_EVENT,   /* Malformed event data */
    MIDI_ERR_END_OF_TRACK,    /* No more events in current track */
    MIDI_ERR_END_OF_FILE,     /* No more tracks to process */
} midi_error_t;

/* MIDI event types (status byte high nibble) */
typedef enum {
    MIDI_STATUS_NOTE_OFF = 0x80,
    MIDI_STATUS_NOTE_ON = 0x90,
    MIDI_STATUS_POLY_PRESSURE = 0xA0,
    MIDI_STATUS_CONTROL_CHANGE = 0xB0,
    MIDI_STATUS_PROGRAM_CHANGE = 0xC0,
    MIDI_STATUS_CHANNEL_PRESSURE = 0xD0,
    MIDI_STATUS_PITCH_BEND = 0xE0,
    MIDI_STATUS_SYSTEM = 0xF0,
} midi_status_t;

/* Meta event types (following 0xFF status) */
typedef enum {
    MIDI_META_SEQUENCE_NUM = 0x00,
    MIDI_META_TEXT = 0x01,
    MIDI_META_COPYRIGHT = 0x02,
    MIDI_META_TRACK_NAME = 0x03,
    MIDI_META_INSTRUMENT = 0x04,
    MIDI_META_LYRIC = 0x05,
    MIDI_META_MARKER = 0x06,
    MIDI_META_CUE_POINT = 0x07,
    MIDI_META_CHANNEL_PREFIX = 0x20,
    MIDI_META_END_OF_TRACK = 0x2F,
    MIDI_META_TEMPO = 0x51,
    MIDI_META_SMPTE_OFFSET = 0x54,
    MIDI_META_TIME_SIG = 0x58,
    MIDI_META_KEY_SIG = 0x59,
    MIDI_META_SEQUENCER_SPECIFIC = 0x7F,
} midi_meta_type_t;

/* Parsed MIDI event */
typedef struct {
    uint32_t delta_time;  /* Delta time in ticks since last event */
    uint32_t abs_time;    /* Absolute time in ticks from track start */
    uint8_t status;       /* Status byte (type | channel for channel msgs) */
    uint8_t type;         /* Event type (status & 0xF0, or meta type) */
    uint8_t channel;      /* Channel (0-15) for channel messages */
    uint8_t data1;        /* First data byte (note, controller, etc.) */
    uint8_t data2;        /* Second data byte (velocity, value, etc.) */
    uint8_t meta_type;    /* Meta event type (when status == 0xFF) */
    uint32_t meta_length; /* Length of meta event data */
    const uint8_t *meta_data; /* Pointer to meta event data (in buffer) */
} midi_event_t;

/* MIDI file header info */
typedef struct {
    uint16_t format;    /* 0 = single track, 1 = multi-track sync, 2 = async */
    uint16_t ntracks;   /* Number of tracks */
    uint16_t division;  /* Ticks per quarter note (if positive) */
    uint8_t uses_smpte; /* Non-zero if SMPTE timing used */
    uint8_t smpte_fps;  /* SMPTE frames per second */
    uint8_t smpte_res;  /* SMPTE ticks per frame */
} midi_header_t;

/* MIDI file parser state */
typedef struct {
    const uint8_t *buffer; /* File data buffer (not owned) */
    size_t buf_len;        /* Total buffer length */
    size_t buf_pos;        /* Current read position */

    midi_header_t header; /* Parsed file header */

    /* Track state */
    uint16_t current_track; /* Current track index (0-based) */
    size_t track_start;     /* Start position of current track data */
    size_t track_end;       /* End position of current track data */
    uint32_t track_time;    /* Accumulated time in current track */
    uint8_t running_status; /* Running status byte */
    uint8_t track_ended;    /* End of track flag */

    /* Tempo tracking (microseconds per quarter note) */
    uint32_t tempo; /* Default: 500000 (120 BPM) */
} midi_file_t;

/**
 * Open and parse MIDI file header.
 * @param mf      Parser state (caller-allocated)
 * @param buffer  MIDI file data (caller retains ownership)
 * @param length  Size of buffer in bytes
 * @return MIDI_OK on success, error code otherwise
 */
midi_error_t midi_file_open(midi_file_t *mf,
                            const uint8_t *buffer,
                            size_t length);

/**
 * Get file header information.
 * @param mf  Parser state
 * @return Pointer to header info (valid while mf is valid)
 */
const midi_header_t *midi_file_get_header(const midi_file_t *mf);

/**
 * Start reading a specific track.
 * @param mf     Parser state
 * @param track  Track index (0-based, must be < ntracks)
 * @return MIDI_OK on success, error code otherwise
 */
midi_error_t midi_file_select_track(midi_file_t *mf, uint16_t track);

/**
 * Read next event from current track.
 * @param mf   Parser state
 * @param evt  Event structure to fill (caller-allocated)
 * @return MIDI_OK on success, MIDI_ERR_END_OF_TRACK when done
 */
midi_error_t midi_file_next_event(midi_file_t *mf, midi_event_t *evt);

/**
 * Convert ticks to milliseconds using current tempo.
 * @param mf     Parser state (for division and tempo)
 * @param ticks  Time in ticks
 * @return Time in milliseconds
 */
uint32_t midi_ticks_to_ms(const midi_file_t *mf, uint32_t ticks);

/**
 * Convert ticks to sample count at given sample rate.
 * @param mf          Parser state (for division and tempo)
 * @param ticks       Time in ticks
 * @param sample_rate Sample rate in Hz
 * @return Time in samples
 */
uint32_t midi_ticks_to_samples(const midi_file_t *mf,
                               uint32_t ticks,
                               uint32_t sample_rate);

/**
 * Check if event is a note-on with velocity > 0.
 * Note: MIDI note-on with velocity 0 is equivalent to note-off.
 */
static inline int midi_is_note_on(const midi_event_t *evt)
{
    return (evt->type == MIDI_STATUS_NOTE_ON) && (evt->data2 > 0);
}

/**
 * Check if event is a note-off (or note-on with velocity 0).
 */
static inline int midi_is_note_off(const midi_event_t *evt)
{
    return (evt->type == MIDI_STATUS_NOTE_OFF) ||
           ((evt->type == MIDI_STATUS_NOTE_ON) && (evt->data2 == 0));
}

/**
 * Get note number from note event.
 */
static inline uint8_t midi_note_number(const midi_event_t *evt)
{
    return evt->data1;
}

/**
 * Get velocity from note event.
 */
static inline uint8_t midi_note_velocity(const midi_event_t *evt)
{
    return evt->data2;
}

#endif /* MIDIFILE_H_ */
