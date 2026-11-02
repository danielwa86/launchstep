#include "app.h"
#include "config.h"
#include "engine.h"
#include "clock.h"
#include "store.h"
#include "ui.h"

// Sparse format "LSQ8":
//
//   0..3   magic 'L' 'S' 'Q' <version>
//   4..5   bpm, two 7-bit halves
//   6      entry velocity, 0 = dynamic mode
//   7      capture quantize, in ticks
//   8      global swing
//   9..14  note-mode setup (root, scale, flags, transpose, channel,
//          octave)                                        (version >= '9')
//   per track: channel, muted, solo, cursor_note, drum_bank (signed),
//              rate, gate, chain_mask, active_pat
//   per track, per pattern:
//     present byte (0 = untouched default pattern, nothing follows)
//     length
//     per step: count, count x (note, velocity, extra)
//               where extra packs the note's nudge and probability
//
// Older saves still load: '2'..'6' (pre-pattern) into pattern 0, and
// '7' (step-level nudge/probability, up to 8 notes per step) with the
// step extras spread onto each note and notes beyond 6 dropped.
// Oversized projects drop notes at save time, never corrupting the
// format.
static const u8 MAGIC[3] = {'L', 'S', 'Q'};

#define STORE_VERSION '9'

// legacy formats allowed up to 8 notes per step
#define LEGACY_NOTES_PER_STEP 8

// build a sanitized extra byte from raw parts
static u8 make_extra(s8 nudge, u8 prob)
{
    if (nudge < SEQ_NUDGE_MIN || nudge > SEQ_NUDGE_MAX)
    {
        nudge = 0;
    }
    if (prob < 1 || prob > SEQ_PROB_MAX)
    {
        prob = SEQ_PROB_MAX;
    }
    return (u8)((((nudge - SEQ_NUDGE_MIN) & 0x07) << 4) | ((prob - 1) & 0x0F));
}

static u8 s_buffer[USER_AREA_SIZE];

static u8 pattern_is_default(const seq_pattern_t *pat)
{
    if (pat->length != SEQ_STEP_COUNT)
    {
        return 0;
    }
    for (u8 s = 0; s < SEQ_STEP_COUNT; ++s)
    {
        for (u8 n = 0; n < SEQ_NOTES_PER_STEP; ++n)
        {
            if (pat->step[s][n].note != SEQ_NOTE_EMPTY)
            {
                return 0;
            }
        }
    }
    return 1;
}

// exact serialized size of a present pattern: present + length bytes,
// one head byte per step, three bytes per placed note
static u16 pattern_bytes(const seq_pattern_t *pat)
{
    u16 size = 2 + SEQ_STEP_COUNT;

    for (u8 s = 0; s < SEQ_STEP_COUNT; ++s)
    {
        for (u8 n = 0; n < SEQ_NOTES_PER_STEP; ++n)
        {
            if (pat->step[s][n].note != SEQ_NOTE_EMPTY)
            {
                size += 3;
            }
        }
    }
    return size;
}

void seq_store_save(void)
{
    u16 pos = 0;

    for (u8 i = 0; i < 3; ++i)
    {
        s_buffer[pos++] = MAGIC[i];
    }
    s_buffer[pos++] = STORE_VERSION;

    u16 bpm = seq_clock_get_bpm();
    s_buffer[pos++] = bpm & 0x7F;
    s_buffer[pos++] = (bpm >> 7) & 0x7F;

    s_buffer[pos++] = seq_ui_get_velocity();
    s_buffer[pos++] = seq_ui_get_quantize();
    s_buffer[pos++] = seq_engine_swing();

    seq_ui_get_note_setup(&s_buffer[pos]);
    pos += NOTE_SETUP_BYTES;

    for (u8 t = 0; t < SEQ_TRACK_COUNT; ++t)
    {
        const seq_track_t *track = &g_seq.track[t];

        s_buffer[pos++] = track->channel;
        s_buffer[pos++] = track->muted;
        s_buffer[pos++] = track->solo;
        s_buffer[pos++] = track->cursor_note;
        s_buffer[pos++] = (u8)track->drum_bank;
        s_buffer[pos++] = track->rate;
        s_buffer[pos++] = track->gate;
        s_buffer[pos++] = track->chain_mask;
        s_buffer[pos++] = track->active_pat;
    }

    // a whole pattern is written only when its exact serialized size
    // fits alongside one absent-marker byte for every pattern still to
    // come - so pos can never pass USER_AREA_SIZE and patterns are
    // dropped whole (never truncated mid-stream)
    u8 remaining = SEQ_TRACK_COUNT * SEQ_PATTERN_COUNT;

    for (u8 t = 0; t < SEQ_TRACK_COUNT; ++t)
    {
        for (u8 p = 0; p < SEQ_PATTERN_COUNT; ++p)
        {
            const seq_pattern_t *pat = &g_seq.track[t].pat[p];

            --remaining;            // patterns after this one

            if (pattern_is_default(pat) ||
                pos + pattern_bytes(pat) + remaining > USER_AREA_SIZE)
            {
                s_buffer[pos++] = 0;
                continue;
            }

            s_buffer[pos++] = 1;
            s_buffer[pos++] = pat->length;

            for (u8 s = 0; s < SEQ_STEP_COUNT; ++s)
            {
                u16 head_pos = pos++;
                u8 count = 0;

                for (u8 n = 0; n < SEQ_NOTES_PER_STEP; ++n)
                {
                    const seq_note_t *slot = &pat->step[s][n];

                    if (slot->note == SEQ_NOTE_EMPTY)
                    {
                        continue;
                    }

                    s_buffer[pos++] = slot->note;
                    s_buffer[pos++] = slot->velocity;
                    s_buffer[pos++] = slot->extra & 0x7F;
                    ++count;
                }

                s_buffer[head_pos] = count;
            }
        }
    }

    hal_write_flash(0, s_buffer, pos);
}

// legacy loader: versions '2'..'6' predate patterns; everything lands in
// pattern 0
static u8 load_legacy(u8 version)
{
    u16 pos = 4;

    u16 bpm = s_buffer[pos] | ((u16)s_buffer[pos + 1] << 7);
    pos += 2;
    seq_clock_set_bpm(bpm);

    s8 global_bank = 0;

    if (version >= '3')
    {
        seq_ui_set_velocity(s_buffer[pos++]);
    }
    if (version == '3')
    {
        global_bank = (s8)s_buffer[pos++];
    }
    if (version >= '5')
    {
        seq_ui_set_quantize(s_buffer[pos++]);
    }

    for (u8 t = 0; t < SEQ_TRACK_COUNT; ++t)
    {
        seq_track_t *track = &g_seq.track[t];

        track->channel = s_buffer[pos++] & 0x0F;
        track->muted = s_buffer[pos++] & 0x01;

        if (version >= '4')
        {
            track->solo = s_buffer[pos++] & 0x01;
            track->cursor_note = s_buffer[pos++] & 0x7F;
            track->drum_bank = (s8)s_buffer[pos++];
        }
        else
        {
            track->cursor_note = s_buffer[pos++] & 0x7F;
            track->drum_bank = global_bank;
        }

        if (version >= '6')
        {
            u8 length = s_buffer[pos++];
            u8 rate = s_buffer[pos++];

            if (length >= 1 && length <= SEQ_STEP_COUNT)
            {
                track->pat[0].length = length;
            }
            seq_engine_set_rate(t, rate);
        }

        if (track->drum_bank < DRUM_BANK_MIN)
        {
            track->drum_bank = DRUM_BANK_MIN;
        }
        if (track->drum_bank > DRUM_BANK_MAX)
        {
            track->drum_bank = DRUM_BANK_MAX;
        }
    }

    for (u8 t = 0; t < SEQ_TRACK_COUNT; ++t)
    {
        seq_pattern_t *pat = &g_seq.track[t].pat[0];

        for (u8 s = 0; s < SEQ_STEP_COUNT; ++s)
        {
            if (pos >= USER_AREA_SIZE)
            {
                return 0;
            }

            u8 count = s_buffer[pos++];

            if (count > LEGACY_NOTES_PER_STEP)
            {
                return 0;
            }

            s8 nudge = 0;

            if (version >= '5')
            {
                if (pos >= USER_AREA_SIZE)
                {
                    return 0;
                }
                nudge = (s8)s_buffer[pos++];
            }

            for (u8 n = 0; n < count; ++n)
            {
                if (pos + 2 > USER_AREA_SIZE)
                {
                    return 0;
                }

                // step-level nudge becomes each note's own; notes beyond
                // today's polyphony are dropped
                if (n < SEQ_NOTES_PER_STEP)
                {
                    pat->step[s][n].note = s_buffer[pos++] & 0x7F;
                    pat->step[s][n].velocity = s_buffer[pos++] & 0x7F;
                    pat->step[s][n].extra = make_extra(nudge, SEQ_PROB_MAX);
                }
                else
                {
                    pos += 2;
                }
            }
        }
    }

    return 1;
}

// loader for the pattern-era formats: '7' (step-level extras, 8 notes)
// and '8' (per-note extras, 6 notes)
static u8 load_modern(u8 version)
{
    u16 pos = 4;

    u16 bpm = s_buffer[pos] | ((u16)s_buffer[pos + 1] << 7);
    pos += 2;
    seq_clock_set_bpm(bpm);

    seq_ui_set_velocity(s_buffer[pos++]);
    seq_ui_set_quantize(s_buffer[pos++]);
    seq_engine_set_swing(s_buffer[pos++]);

    if (version >= '9')
    {
        seq_ui_set_note_setup(&s_buffer[pos]);
        pos += NOTE_SETUP_BYTES;
    }

    for (u8 t = 0; t < SEQ_TRACK_COUNT; ++t)
    {
        seq_track_t *track = &g_seq.track[t];

        track->channel = s_buffer[pos++] & 0x0F;
        track->muted = s_buffer[pos++] & 0x01;
        track->solo = s_buffer[pos++] & 0x01;
        track->cursor_note = s_buffer[pos++] & 0x7F;
        track->drum_bank = (s8)s_buffer[pos++];
        seq_engine_set_rate(t, s_buffer[pos++]);
        track->gate = s_buffer[pos++];
        track->chain_mask = s_buffer[pos++] & 0x0F;
        track->active_pat = s_buffer[pos++] % SEQ_PATTERN_COUNT;
        track->edit_pat = track->active_pat;

        if (track->gate < 1 || track->gate > 4)
        {
            track->gate = SEQ_DEFAULT_GATE;
        }
        if (!track->chain_mask)
        {
            track->chain_mask = 1;
        }
        if (track->drum_bank < DRUM_BANK_MIN)
        {
            track->drum_bank = DRUM_BANK_MIN;
        }
        if (track->drum_bank > DRUM_BANK_MAX)
        {
            track->drum_bank = DRUM_BANK_MAX;
        }
    }

    for (u8 t = 0; t < SEQ_TRACK_COUNT; ++t)
    {
        for (u8 p = 0; p < SEQ_PATTERN_COUNT; ++p)
        {
            if (pos >= USER_AREA_SIZE)
            {
                return 0;
            }

            if (!s_buffer[pos++])
            {
                continue;           // default pattern, already initialised
            }

            seq_pattern_t *pat = &g_seq.track[t].pat[p];

            if (pos >= USER_AREA_SIZE)
            {
                return 0;
            }

            u8 length = s_buffer[pos++];

            if (length >= 1 && length <= SEQ_STEP_COUNT)
            {
                pat->length = length;
            }

            for (u8 s = 0; s < SEQ_STEP_COUNT; ++s)
            {
                if (pos >= USER_AREA_SIZE)
                {
                    return 0;
                }

                u8 head = s_buffer[pos++];
                u8 count = head & 0x7F;

                if (count > LEGACY_NOTES_PER_STEP)
                {
                    return 0;
                }

                s8 snudge = 0;
                u8 sprob = SEQ_PROB_MAX;

                // v7 carried step-level extras behind a flag bit
                if (version == '7' && (head & 0x80))
                {
                    if (pos + 2 > USER_AREA_SIZE)
                    {
                        return 0;
                    }
                    snudge = (s8)s_buffer[pos++];
                    sprob = s_buffer[pos++];
                }

                u8 note_bytes = (version >= '8') ? 3 : 2;

                for (u8 n = 0; n < count; ++n)
                {
                    if (pos + note_bytes > USER_AREA_SIZE)
                    {
                        return 0;
                    }

                    if (n < SEQ_NOTES_PER_STEP)
                    {
                        seq_note_t *slot = &pat->step[s][n];

                        slot->note = s_buffer[pos++] & 0x7F;
                        slot->velocity = s_buffer[pos++] & 0x7F;

                        if (version >= '8')
                        {
                            u8 extra = s_buffer[pos++] & 0x7F;

                            // sanitize through unpack/repack
                            slot->extra = make_extra(
                                (s8)(((extra >> 4) & 0x07) + SEQ_NUDGE_MIN),
                                (u8)((extra & 0x0F) + 1));
                        }
                        else
                        {
                            slot->extra = make_extra(snudge, sprob);
                        }
                    }
                    else
                    {
                        pos += note_bytes;      // beyond today's polyphony
                    }
                }
            }
        }
    }

    return 1;
}

u8 seq_store_load(void)
{
    hal_read_flash(0, s_buffer, USER_AREA_SIZE);

    for (u8 i = 0; i < 3; ++i)
    {
        if (s_buffer[i] != MAGIC[i])
        {
            return 0;
        }
    }

    u8 version = s_buffer[3];

    if (version < '2' || version > STORE_VERSION)
    {
        return 0;
    }

    u8 ok = (version >= '7') ? load_modern(version) : load_legacy(version);

    if (!ok)
    {
        // corrupt save - discard the partial load COMPLETELY, including
        // header values (bpm/velocity/quantize/note setup) already applied
        seq_engine_init();
        seq_clock_set_bpm(SEQ_DEFAULT_BPM);
        seq_ui_set_velocity(SEQ_DEFAULT_VELOCITY);
        seq_ui_set_quantize(SEQ_QUANT_16TH);

        static const u8 NOTE_DEFAULTS[NOTE_SETUP_BYTES] = {0, 0, 2, 0, 0, 0};

        seq_ui_set_note_setup(NOTE_DEFAULTS);
        return 0;
    }

    seq_engine_rebuild_note_usage();
    return 1;
}
