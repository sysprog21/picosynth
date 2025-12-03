/* MIDI file parser tests */

#include <string.h>
#include "midifile.h"
#include "test.h"

/* Minimal valid MIDI file: MThd + empty MTrk */
static const uint8_t midi_minimal[] = {
    /* MThd header */
    'M', 'T', 'h', 'd', 0, 0, 0, 6, /* chunk length */
    0, 0,                           /* format 0 */
    0, 1,                           /* 1 track */
    0x01, 0xE0,                     /* 480 ticks per quarter */
    /* MTrk track */
    'M', 'T', 'r', 'k', 0, 0, 0, 4, /* track length */
    0x00, 0xFF, 0x2F, 0x00          /* end of track */
};

/* MIDI file with single note (C4, quarter note at 120 BPM) */
static const uint8_t midi_single_note[] = {
    /* MThd header */
    'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, /* format 0 */
    0, 1,                                 /* 1 track */
    0x01, 0xE0,                           /* 480 ticks per quarter */
    /* MTrk track */
    'M', 'T', 'r', 'k', 0, 0, 0, 19, /* track length */
    /* Tempo: 500000 us/quarter = 120 BPM */
    0x00, 0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20,
    /* Note on: delta=0, channel 0, note 60, velocity 100 */
    0x00, 0x90, 0x3C, 0x64,
    /* Note off: delta=480 (0x83 0x60), channel 0, note 60, velocity 0 */
    0x83, 0x60, 0x80, 0x3C, 0x00,
    /* End of track */
    0x00, 0xFF, 0x2F, 0x00};

/* MIDI file with C major scale (C4-C5) */
static const uint8_t midi_scale[] = {
    /* MThd header */
    'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, /* format 0 */
    0, 1,                                 /* 1 track */
    0x01, 0xE0,                           /* 480 ticks per quarter */
    /* MTrk track */
    'M', 'T', 'r', 'k', 0, 0, 0, 75, /* track length */
    /* Tempo: 500000 us/quarter = 120 BPM */
    0x00, 0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20,
    /* C4 */
    0x00, 0x90, 60, 100, 0x83, 0x60, 0x80, 60, 0,
    /* D4 */
    0x00, 0x90, 62, 100, 0x83, 0x60, 0x80, 62, 0,
    /* E4 */
    0x00, 0x90, 64, 100, 0x83, 0x60, 0x80, 64, 0,
    /* F4 */
    0x00, 0x90, 65, 100, 0x83, 0x60, 0x80, 65, 0,
    /* G4 */
    0x00, 0x90, 67, 100, 0x83, 0x60, 0x80, 67, 0,
    /* A4 */
    0x00, 0x90, 69, 100, 0x83, 0x60, 0x80, 69, 0,
    /* B4 */
    0x00, 0x90, 71, 100, 0x83, 0x60, 0x80, 71, 0,
    /* C5 */
    0x00, 0x90, 72, 100, 0x83, 0x60, 0x80, 72, 0,
    /* End of track */
    0x00, 0xFF, 0x2F, 0x00};

/* Invalid: not a MIDI file */
static const uint8_t invalid_not_midi[] = {'R', 'I', 'F', 'F', /* WAV header */
                                           0,   0,   0,   0,
                                           'W', 'A', 'V', 'E'};

/* Invalid: truncated header */
static const uint8_t invalid_truncated[] = {
    'M', 'T', 'h', 'd', 0,
    0,   0,   6
    /* Missing header data and tracks */
};

/* Invalid: oversized chunk length in header */
static const uint8_t invalid_oversized_header[] = {
    'M',  'T',  'h',  'd',
    0xFF, 0xFF, 0xFF, 0xFF, /* Chunk length larger than buffer */
    0,    0,                /* format 0 */
    0,    1,                /* 1 track */
    0x01, 0xE0              /* 480 ticks per quarter */
};

/* Invalid: oversized track chunk length */
static const uint8_t invalid_oversized_track[] = {
    /* MThd header */
    'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, /* format 0 */
    0, 1,                                 /* 1 track */
    0x01, 0xE0,                           /* 480 ticks per quarter */
    /* MTrk track with oversized length */
    'M', 'T', 'r', 'k', 0xFF, 0xFF, 0xFF,
    0xFF /* Chunk length larger than buffer */
};

/* Invalid: unterminated VLQ (5+ continuation bytes) */
static const uint8_t invalid_vlq[] = {
    /* MThd header */
    'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, /* format 0 */
    0, 1,                                 /* 1 track */
    0x01, 0xE0,                           /* 480 ticks per quarter */
    /* MTrk track */
    'M', 'T', 'r', 'k', 0, 0, 0, 8, /* track length */
    /* VLQ with too many continuation bytes (5 bytes with MSB set) */
    0x80, 0x80, 0x80, 0x80, 0x80, 0x00, 0xFF, 0x2F, 0x00 /* end of track */
};

/* MIDI file with system common message (Song Position Pointer) */
static const uint8_t midi_system_common[] = {
    /* MThd header */
    'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, /* format 0 */
    0, 1,                                 /* 1 track */
    0x00, 0x60,                           /* 96 ticks per quarter */
    /* MTrk track */
    'M', 'T', 'r', 'k', 0, 0, 0, 15, /* track length */
    /* Song Position Pointer (0xF2) with 2 data bytes */
    0x00, 0xF2, 0x10, 0x20,
    /* Note on C4 */
    0x00, 0x90, 60, 100,
    /* Note off C4 */
    0x60, 0x80, 60, 0,
    /* End of track */
    0x00, 0xFF, 0x2F, 0x00};

/* MIDI file using running status */
static const uint8_t midi_running_status[] = {
    /* MThd header */
    'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, /* format 0 */
    0, 1,                                 /* 1 track */
    0x00, 0x60,                           /* 96 ticks per quarter */
    /* MTrk track */
    'M', 'T', 'r', 'k', 0, 0, 0, 16, /* track length */
    /* Note on C4 */
    0x00, 0x90, 60, 100,
    /* Note on D4 using running status (no status byte) */
    0x60, 62, 100,
    /* Note off C4 */
    0x00, 0x80, 60, 0,
    /* Note off D4 using running status */
    0x60, 62, 0,
    /* End of track */
    0x00, 0xFF, 0x2F, 0x00};

static void test_midi_open_valid(void)
{
    midi_file_t mf;
    midi_error_t err;

    err = midi_file_open(&mf, midi_minimal, sizeof(midi_minimal));
    TEST_ASSERT_EQ(err, MIDI_OK, "open minimal MIDI file");
    TEST_ASSERT_EQ(mf.header.format, 0, "format 0");
    TEST_ASSERT_EQ(mf.header.ntracks, 1, "1 track");
    TEST_ASSERT_EQ(mf.header.division, 480, "480 ticks/quarter");
    TEST_ASSERT_EQ(mf.header.uses_smpte, 0, "not SMPTE");
}

static void test_midi_open_invalid(void)
{
    midi_file_t mf;
    midi_error_t err;

    err = midi_file_open(&mf, invalid_not_midi, sizeof(invalid_not_midi));
    TEST_ASSERT(err != MIDI_OK, "reject non-MIDI file");

    err = midi_file_open(&mf, invalid_truncated, sizeof(invalid_truncated));
    TEST_ASSERT(err != MIDI_OK, "reject truncated file");

    err = midi_file_open(&mf, NULL, 0);
    TEST_ASSERT(err != MIDI_OK, "reject NULL buffer");

    /* Test oversized header chunk length */
    err = midi_file_open(&mf, invalid_oversized_header,
                         sizeof(invalid_oversized_header));
    TEST_ASSERT(err != MIDI_OK, "reject oversized header chunk");
}

static void test_midi_oversized_track(void)
{
    midi_file_t mf;
    midi_error_t err;

    err = midi_file_open(&mf, invalid_oversized_track,
                         sizeof(invalid_oversized_track));
    TEST_ASSERT_EQ(err, MIDI_OK, "open file with oversized track");

    /* Track selection should fail due to oversized chunk */
    err = midi_file_select_track(&mf, 0);
    TEST_ASSERT(err != MIDI_OK, "reject oversized track chunk");
}

static void test_midi_invalid_vlq(void)
{
    midi_file_t mf;
    midi_event_t evt;
    midi_error_t err;

    err = midi_file_open(&mf, invalid_vlq, sizeof(invalid_vlq));
    TEST_ASSERT_EQ(err, MIDI_OK, "open file with invalid VLQ");

    err = midi_file_select_track(&mf, 0);
    TEST_ASSERT_EQ(err, MIDI_OK, "select track with invalid VLQ");

    /* Reading event should fail due to invalid VLQ */
    err = midi_file_next_event(&mf, &evt);
    TEST_ASSERT(err != MIDI_OK, "reject invalid VLQ");
}

static void test_midi_system_common(void)
{
    midi_file_t mf;
    midi_event_t evt;
    midi_error_t err;
    int note_on_count = 0;
    int note_off_count = 0;
    int system_msg_count = 0;

    err = midi_file_open(&mf, midi_system_common, sizeof(midi_system_common));
    TEST_ASSERT_EQ(err, MIDI_OK, "open system common file");

    err = midi_file_select_track(&mf, 0);
    TEST_ASSERT_EQ(err, MIDI_OK, "select track");

    while ((err = midi_file_next_event(&mf, &evt)) == MIDI_OK) {
        if (midi_is_note_on(&evt))
            note_on_count++;
        else if (midi_is_note_off(&evt))
            note_off_count++;
        else if (evt.type == 0xF2)
            system_msg_count++;
    }

    TEST_ASSERT_EQ(system_msg_count, 1, "1 system common event");
    TEST_ASSERT_EQ(note_on_count, 1, "1 note-on after system common");
    TEST_ASSERT_EQ(note_off_count, 1, "1 note-off after system common");
}

static void test_midi_select_track(void)
{
    midi_file_t mf;
    midi_error_t err;

    err = midi_file_open(&mf, midi_single_note, sizeof(midi_single_note));
    TEST_ASSERT_EQ(err, MIDI_OK, "open single note file");

    err = midi_file_select_track(&mf, 0);
    TEST_ASSERT_EQ(err, MIDI_OK, "select track 0");

    err = midi_file_select_track(&mf, 1);
    TEST_ASSERT_EQ(err, MIDI_ERR_INVALID_TRACK, "reject invalid track");
}

static void test_midi_single_note(void)
{
    midi_file_t mf;
    midi_event_t evt;
    midi_error_t err;
    int note_on_count = 0;
    int note_off_count = 0;
    uint8_t note_number = 0;

    err = midi_file_open(&mf, midi_single_note, sizeof(midi_single_note));
    TEST_ASSERT_EQ(err, MIDI_OK, "open single note file");

    err = midi_file_select_track(&mf, 0);
    TEST_ASSERT_EQ(err, MIDI_OK, "select track 0");

    while ((err = midi_file_next_event(&mf, &evt)) == MIDI_OK) {
        if (midi_is_note_on(&evt)) {
            note_on_count++;
            note_number = midi_note_number(&evt);
        } else if (midi_is_note_off(&evt)) {
            note_off_count++;
        }
    }

    TEST_ASSERT_EQ(note_on_count, 1, "one note-on event");
    TEST_ASSERT_EQ(note_off_count, 1, "one note-off event");
    TEST_ASSERT_EQ(note_number, 60, "note is C4 (MIDI 60)");
}

static void test_midi_scale(void)
{
    midi_file_t mf;
    midi_event_t evt;
    midi_error_t err;
    int note_count = 0;
    uint8_t expected_notes[] = {60, 62, 64, 65, 67, 69, 71, 72};
    uint8_t actual_notes[8] = {0};

    err = midi_file_open(&mf, midi_scale, sizeof(midi_scale));
    TEST_ASSERT_EQ(err, MIDI_OK, "open scale file");

    err = midi_file_select_track(&mf, 0);
    TEST_ASSERT_EQ(err, MIDI_OK, "select track 0");

    while ((err = midi_file_next_event(&mf, &evt)) == MIDI_OK) {
        if (midi_is_note_on(&evt) && note_count < 8) {
            actual_notes[note_count++] = midi_note_number(&evt);
        }
    }

    TEST_ASSERT_EQ(note_count, 8, "8 notes in scale");
    for (int i = 0; i < 8; i++) {
        char msg[64];
        snprintf(msg, sizeof(msg), "scale note %d correct", i);
        TEST_ASSERT_EQ(actual_notes[i], expected_notes[i], msg);
    }
}

static void test_midi_running_status(void)
{
    midi_file_t mf;
    midi_event_t evt;
    midi_error_t err;
    int note_on_count = 0;
    int note_off_count = 0;

    err = midi_file_open(&mf, midi_running_status, sizeof(midi_running_status));
    TEST_ASSERT_EQ(err, MIDI_OK, "open running status file");

    err = midi_file_select_track(&mf, 0);
    TEST_ASSERT_EQ(err, MIDI_OK, "select track 0");

    while ((err = midi_file_next_event(&mf, &evt)) == MIDI_OK) {
        if (midi_is_note_on(&evt))
            note_on_count++;
        else if (midi_is_note_off(&evt))
            note_off_count++;
    }

    TEST_ASSERT_EQ(note_on_count, 2, "2 note-on events (running status)");
    TEST_ASSERT_EQ(note_off_count, 2, "2 note-off events (running status)");
}

static void test_midi_timing(void)
{
    midi_file_t mf;
    midi_event_t evt;
    midi_error_t err;

    err = midi_file_open(&mf, midi_single_note, sizeof(midi_single_note));
    TEST_ASSERT_EQ(err, MIDI_OK, "open file for timing test");

    err = midi_file_select_track(&mf, 0);
    TEST_ASSERT_EQ(err, MIDI_OK, "select track");

    /* Skip tempo event */
    midi_file_next_event(&mf, &evt);

    /* Note on at time 0 */
    err = midi_file_next_event(&mf, &evt);
    TEST_ASSERT_EQ(err, MIDI_OK, "read note-on");
    TEST_ASSERT_EQ(evt.abs_time, 0, "note-on at time 0");

    /* Note off at time 480 (quarter note at 480 ticks/quarter) */
    err = midi_file_next_event(&mf, &evt);
    TEST_ASSERT_EQ(err, MIDI_OK, "read note-off");
    TEST_ASSERT_EQ(evt.abs_time, 480, "note-off at time 480");

    /* Convert ticks to milliseconds: 480 ticks at 120 BPM = 500ms */
    uint32_t ms = midi_ticks_to_ms(&mf, 480);
    TEST_ASSERT_EQ(ms, 500, "480 ticks = 500ms at 120 BPM");
}

static void test_midi_ticks_to_samples(void)
{
    midi_file_t mf;
    midi_error_t err;

    err = midi_file_open(&mf, midi_single_note, sizeof(midi_single_note));
    TEST_ASSERT_EQ(err, MIDI_OK, "open file for samples test");

    /* At 120 BPM, 480 ticks = 500ms = 5513 samples at 11025 Hz */
    uint32_t samples = midi_ticks_to_samples(&mf, 480, 11025);
    TEST_ASSERT_RANGE(samples, 5510, 5515, "samples at 11025 Hz");

    /* At 44100 Hz, 500ms = 22050 samples */
    samples = midi_ticks_to_samples(&mf, 480, 44100);
    TEST_ASSERT_RANGE(samples, 22048, 22052, "samples at 44100 Hz");
}

static void test_midi_helper_functions(void)
{
    midi_event_t evt;

    /* Note-on event */
    memset(&evt, 0, sizeof(evt));
    evt.type = MIDI_STATUS_NOTE_ON;
    evt.data1 = 60;  /* note */
    evt.data2 = 100; /* velocity */
    TEST_ASSERT(midi_is_note_on(&evt), "note-on detected");
    TEST_ASSERT(!midi_is_note_off(&evt), "not note-off");
    TEST_ASSERT_EQ(midi_note_number(&evt), 60, "note number");
    TEST_ASSERT_EQ(midi_note_velocity(&evt), 100, "velocity");

    /* Note-on with velocity 0 (equivalent to note-off) */
    evt.data2 = 0;
    TEST_ASSERT(!midi_is_note_on(&evt), "vel=0 not note-on");
    TEST_ASSERT(midi_is_note_off(&evt), "vel=0 is note-off");

    /* Note-off event */
    evt.type = MIDI_STATUS_NOTE_OFF;
    evt.data2 = 64;
    TEST_ASSERT(!midi_is_note_on(&evt), "note-off not note-on");
    TEST_ASSERT(midi_is_note_off(&evt), "note-off detected");
}

void test_midi_all(void)
{
    test_midi_open_valid();
    test_midi_open_invalid();
    test_midi_oversized_track();
    test_midi_invalid_vlq();
    test_midi_system_common();
    test_midi_select_track();
    test_midi_single_note();
    test_midi_scale();
    test_midi_running_status();
    test_midi_timing();
    test_midi_ticks_to_samples();
    test_midi_helper_functions();
}
