/*
 * midifile.c - Lightweight MIDI File Decoder
 *
 * Parses Standard MIDI File (SMF) format per MIDI 1.0 specification.
 * Supports format 0 (single track) and format 1 (multi-track synchronous).
 *
 * Reference: MIDI 1.0 Detailed Specification, RP-001 (midi.org)
 */

#include "midifile.h"
#include <string.h>

/* MIDI file chunk IDs */
#define MIDI_CHUNK_MTHD 0x4D546864 /* "MThd" */
#define MIDI_CHUNK_MTRK 0x4D54726B /* "MTrk" */

/* Default tempo: 500000 us/quarter = 120 BPM */
#define MIDI_DEFAULT_TEMPO 500000

/* Read big-endian 16-bit value from buffer */
static uint16_t read_be16(const uint8_t *p)
{
    return (uint16_t) ((uint16_t) p[0] << 8 | (uint16_t) p[1]);
}

/* Read big-endian 32-bit value from buffer */
static uint32_t read_be32(const uint8_t *p)
{
    return (uint32_t) p[0] << 24 | (uint32_t) p[1] << 16 |
           (uint32_t) p[2] << 8 | (uint32_t) p[3];
}

/* Read big-endian 24-bit value from buffer (for tempo) */
static uint32_t read_be24(const uint8_t *p)
{
    return (uint32_t) p[0] << 16 | (uint32_t) p[1] << 8 | (uint32_t) p[2];
}

/* Read variable-length quantity (VLQ).
 * VLQ uses 7 bits per byte, with MSB set on all bytes except the last.
 * Returns number of bytes read, or 0 on error.
 */
static size_t read_vlq(const uint8_t *p, size_t max_len, uint32_t *value)
{
    uint32_t result = 0;
    size_t i;

    for (i = 0; i < max_len && i < 4; i++) {
        result = (result << 7) | (uint32_t) (p[i] & 0x7F);
        if ((p[i] & 0x80) == 0) {
            *value = result;
            return i + 1;
        }
    }

    /* VLQ too long or truncated */
    return 0;
}

/* Get number of data bytes for a channel message type.
 * Returns 0, 1, or 2.
 */
static int channel_msg_length(uint8_t status)
{
    switch (status & 0xF0) {
    case MIDI_STATUS_NOTE_OFF:
    case MIDI_STATUS_NOTE_ON:
    case MIDI_STATUS_POLY_PRESSURE:
    case MIDI_STATUS_CONTROL_CHANGE:
    case MIDI_STATUS_PITCH_BEND:
        return 2;
    case MIDI_STATUS_PROGRAM_CHANGE:
    case MIDI_STATUS_CHANNEL_PRESSURE:
        return 1;
    default:
        return 0;
    }
}

midi_error_t midi_file_open(midi_file_t *mf,
                            const uint8_t *buffer,
                            size_t length)
{
    uint32_t chunk_id, chunk_len;
    int16_t division_raw;

    if (!mf || !buffer)
        return MIDI_ERR_INVALID_HEADER;

    memset(mf, 0, sizeof(midi_file_t));
    mf->buffer = buffer;
    mf->buf_len = length;
    mf->buf_pos = 0;
    mf->tempo = MIDI_DEFAULT_TEMPO;

    /* Need at least 14 bytes for MThd header */
    if (length < 14)
        return MIDI_ERR_TRUNCATED;

    /* Read chunk header */
    chunk_id = read_be32(buffer);
    chunk_len = read_be32(buffer + 4);

    if (chunk_id != MIDI_CHUNK_MTHD)
        return MIDI_ERR_INVALID_HEADER;

    /* Standard MThd is 6 bytes, but spec allows longer */
    if (chunk_len < 6)
        return MIDI_ERR_INVALID_HEADER;

    /* Validate chunk_len doesn't exceed buffer */
    if (chunk_len > length - 8)
        return MIDI_ERR_TRUNCATED;

    /* Parse header data */
    mf->header.format = read_be16(buffer + 8);
    mf->header.ntracks = read_be16(buffer + 10);
    division_raw = (int16_t) read_be16(buffer + 12);

    /* Check format type */
    if (mf->header.format > 1)
        return MIDI_ERR_UNSUPPORTED_FMT; /* Format 2 not supported */

    /* Parse division (timing) */
    if (division_raw < 0) {
        /* SMPTE timing */
        mf->header.uses_smpte = 1;
        mf->header.smpte_fps = (uint8_t) (-(division_raw >> 8));
        mf->header.smpte_res = (uint8_t) (division_raw & 0xFF);
        mf->header.division =
            (uint16_t) (mf->header.smpte_fps * mf->header.smpte_res);
    } else {
        /* Ticks per quarter note */
        mf->header.uses_smpte = 0;
        mf->header.division = (uint16_t) division_raw;
    }

    /* Move past MThd chunk */
    mf->buf_pos = 8 + chunk_len;

    return MIDI_OK;
}

const midi_header_t *midi_file_get_header(const midi_file_t *mf)
{
    return mf ? &mf->header : NULL;
}

midi_error_t midi_file_select_track(midi_file_t *mf, uint16_t track)
{
    uint32_t chunk_id, chunk_len;
    size_t pos;
    uint16_t track_count = 0;

    if (!mf)
        return MIDI_ERR_INVALID_HEADER;

    if (track >= mf->header.ntracks)
        return MIDI_ERR_INVALID_TRACK;

    /* Start from after MThd header and scan for MTrk chunks */
    pos = 8 + (mf->buf_pos > 8 ? 6 : 0); /* Reset to after MThd */

    /* Re-scan from beginning to find first track */
    pos = 8;
    if (mf->buf_len >= 14) {
        chunk_len = read_be32(mf->buffer + 4);
        /* Guard against overflow when computing pos */
        if (chunk_len > mf->buf_len - 8)
            return MIDI_ERR_TRUNCATED;
        pos = 8 + chunk_len;
    }

    while (pos + 8 <= mf->buf_len) {
        chunk_id = read_be32(mf->buffer + pos);
        chunk_len = read_be32(mf->buffer + pos + 4);

        /* Guard against chunk_len overflow */
        if (chunk_len > mf->buf_len - pos - 8)
            return MIDI_ERR_TRUNCATED;

        if (chunk_id == MIDI_CHUNK_MTRK) {
            if (track_count == track) {
                /* Found the requested track */
                mf->current_track = track;
                mf->track_start = pos + 8;
                mf->track_end = pos + 8 + chunk_len;
                mf->buf_pos = pos + 8;
                mf->track_time = 0;
                mf->running_status = 0;
                mf->track_ended = 0;

                if (mf->track_end > mf->buf_len)
                    return MIDI_ERR_TRUNCATED;

                return MIDI_OK;
            }
            track_count++;
        }

        pos += 8 + chunk_len;
    }

    return MIDI_ERR_INVALID_TRACK;
}

midi_error_t midi_file_next_event(midi_file_t *mf, midi_event_t *evt)
{
    uint32_t delta;
    size_t vlq_len;
    uint8_t status;
    size_t remaining;

    if (!mf || !evt)
        return MIDI_ERR_INVALID_EVENT;

    if (mf->track_ended)
        return MIDI_ERR_END_OF_TRACK;

    if (mf->buf_pos >= mf->track_end)
        return MIDI_ERR_END_OF_TRACK;

    remaining = mf->track_end - mf->buf_pos;

    /* Read delta time */
    vlq_len = read_vlq(mf->buffer + mf->buf_pos, remaining, &delta);
    if (vlq_len == 0)
        return MIDI_ERR_TRUNCATED;

    mf->buf_pos += vlq_len;
    remaining -= vlq_len;

    if (remaining == 0)
        return MIDI_ERR_TRUNCATED;

    /* Initialize event */
    memset(evt, 0, sizeof(midi_event_t));
    evt->delta_time = delta;

    /* Guard against track_time overflow */
    if (delta > UINT32_MAX - mf->track_time)
        return MIDI_ERR_INVALID_EVENT;

    mf->track_time += delta;
    evt->abs_time = mf->track_time;

    /* Read status byte */
    status = mf->buffer[mf->buf_pos];

    if (status & 0x80) {
        /* New status byte */
        mf->buf_pos++;
        remaining--;

        if (status < 0xF0) {
            /* Channel message - update running status */
            mf->running_status = status;
        }
    } else {
        /* Running status */
        if (mf->running_status == 0)
            return MIDI_ERR_INVALID_EVENT;
        status = mf->running_status;
    }

    evt->status = status;

    /* Parse based on status type */
    if (status < 0xF0) {
        /* Channel message */
        int data_len = channel_msg_length(status);
        evt->type = status & 0xF0;
        evt->channel = status & 0x0F;

        if (remaining < (size_t) data_len)
            return MIDI_ERR_TRUNCATED;

        if (data_len >= 1) {
            evt->data1 = mf->buffer[mf->buf_pos++];
            remaining--;
        }
        if (data_len >= 2) {
            evt->data2 = mf->buffer[mf->buf_pos++];
            remaining--;
        }
    } else if (status == 0xFF) {
        /* Meta event */
        uint32_t meta_len;

        if (remaining < 1)
            return MIDI_ERR_TRUNCATED;

        evt->meta_type = mf->buffer[mf->buf_pos++];
        remaining--;

        vlq_len = read_vlq(mf->buffer + mf->buf_pos, remaining, &meta_len);
        if (vlq_len == 0)
            return MIDI_ERR_TRUNCATED;

        mf->buf_pos += vlq_len;
        remaining -= vlq_len;

        if (remaining < meta_len)
            return MIDI_ERR_TRUNCATED;

        evt->meta_length = meta_len;
        evt->meta_data = mf->buffer + mf->buf_pos;
        evt->type = 0xFF;

        /* Handle tempo changes */
        if (evt->meta_type == MIDI_META_TEMPO && meta_len == 3) {
            mf->tempo = read_be24(evt->meta_data);
        }

        /* Check for end of track */
        if (evt->meta_type == MIDI_META_END_OF_TRACK) {
            mf->track_ended = 1;
        }

        mf->buf_pos += meta_len;
    } else if (status == 0xF0 || status == 0xF7) {
        /* SysEx event */
        uint32_t sysex_len;

        vlq_len = read_vlq(mf->buffer + mf->buf_pos, remaining, &sysex_len);
        if (vlq_len == 0)
            return MIDI_ERR_TRUNCATED;

        mf->buf_pos += vlq_len;
        remaining -= vlq_len;

        if (remaining < sysex_len)
            return MIDI_ERR_TRUNCATED;

        evt->type = status;
        evt->meta_length = sysex_len;
        evt->meta_data = mf->buffer + mf->buf_pos;

        mf->buf_pos += sysex_len;

        /* Clear running status after SysEx */
        mf->running_status = 0;
    } else {
        /* System common messages - consume their data bytes properly */
        evt->type = status;
        mf->running_status = 0;

        switch (status) {
        case 0xF1: /* MIDI Time Code Quarter Frame - 1 data byte */
        case 0xF3: /* Song Select - 1 data byte */
            if (remaining < 1)
                return MIDI_ERR_TRUNCATED;
            evt->data1 = mf->buffer[mf->buf_pos++];
            break;
        case 0xF2: /* Song Position Pointer - 2 data bytes */
            if (remaining < 2)
                return MIDI_ERR_TRUNCATED;
            evt->data1 = mf->buffer[mf->buf_pos++];
            evt->data2 = mf->buffer[mf->buf_pos++];
            break;
        case 0xF6: /* Tune Request - no data */
        case 0xF8: /* Timing Clock - no data */
        case 0xFA: /* Start - no data */
        case 0xFB: /* Continue - no data */
        case 0xFC: /* Stop - no data */
        case 0xFE: /* Active Sensing - no data */
            /* No data bytes to consume */
            break;
        default:
            /* Unknown system message - return error */
            return MIDI_ERR_INVALID_EVENT;
        }
    }

    return MIDI_OK;
}

uint32_t midi_ticks_to_ms(const midi_file_t *mf, uint32_t ticks)
{
    uint64_t us;
    uint64_t ms;

    if (!mf || mf->header.division == 0)
        return 0;

    if (mf->header.uses_smpte) {
        /* SMPTE: ticks / (fps * res) * 1000 */
        us = ((uint64_t) ticks * 1000000) / mf->header.division;
    } else {
        /* Ticks per quarter note: ticks * tempo / division */
        us = ((uint64_t) ticks * mf->tempo) / mf->header.division;
    }

    ms = us / 1000;
    /* Saturate to UINT32_MAX on overflow */
    return ms > UINT32_MAX ? UINT32_MAX : (uint32_t) ms;
}

uint32_t midi_ticks_to_samples(const midi_file_t *mf,
                               uint32_t ticks,
                               uint32_t sample_rate)
{
    uint64_t us;
    uint64_t samples;

    if (!mf || mf->header.division == 0 || sample_rate == 0)
        return 0;

    if (mf->header.uses_smpte) {
        us = ((uint64_t) ticks * 1000000) / mf->header.division;
    } else {
        us = ((uint64_t) ticks * mf->tempo) / mf->header.division;
    }

    /* Convert microseconds to samples with overflow protection */
    /* Check if multiplication would overflow uint64_t */
    if (us > UINT64_MAX / sample_rate) {
        /* Use alternative calculation to avoid overflow */
        samples = (us / 1000000) * sample_rate +
                  ((us % 1000000) * sample_rate) / 1000000;
    } else {
        samples = (us * sample_rate) / 1000000;
    }

    /* Saturate to UINT32_MAX on overflow */
    return samples > UINT32_MAX ? UINT32_MAX : (uint32_t) samples;
}
