#include "app.h"
#include "engine.h"

seq_engine_t g_seq;

// per-slot gate countdown to each sounding note's note-off (0 = free),
// and the note/channel it was fired with, so note-offs always match
// their note-ons even if the pattern or channel changes mid-note
static u8 s_gate_left[SEQ_TRACK_COUNT][SEQ_NOTES_PER_STEP];
static u8 s_sounding_note[SEQ_TRACK_COUNT][SEQ_NOTES_PER_STEP];
static u8 s_sounding_ch[SEQ_TRACK_COUNT][SEQ_NOTES_PER_STEP];

// per-track playheads: position and tick-within-step (polymeter)
static u8 s_pos[SEQ_TRACK_COUNT];
static u8 s_tick[SEQ_TRACK_COUNT];

// pattern ends passed per track (for the UI's capture auto-disarm)
static u8 s_wraps[SEQ_TRACK_COUNT];

// set at start/boundary-seek: the first boundary scan also fires the
// step's early-shifted notes, whose window lay before time began
static u8 s_catchup[SEQ_TRACK_COUNT];

// global bar phase for fills and quantized starts, 0 = the "1"
static u8 s_bar_tick;

static u8 s_swing;

// xorshift PRNG for note probability
static u32 s_rng = 0x9E3779B9UL;

const u8 SEQ_RATES[SEQ_RATE_COUNT] = {3, 4, 6, 8, 12, 16, 24, 48};

// beat repeat / fill state
static u8 s_fill_rate;              // ticks between retriggers, 0 = off
static u8 s_fill_held;
static u8 s_fill_phase;

// the last step each track actually triggered, and the pattern it lives
// in - the material a fill repeats
static u8 s_last_step[SEQ_TRACK_COUNT];
static u8 s_last_pat[SEQ_TRACK_COUNT];

#define NO_STEP 0xFF

// kick, snare, closed hat, open hat: the initial note lane per track
static const u8 DEFAULT_NOTES[SEQ_TRACK_COUNT] = {36, 38, 42, 46};

// ____________________________________________________________________________
//
// Per-note extras: nudge and probability packed into one byte
// ____________________________________________________________________________

static u8 pack_extra(s8 nudge, u8 prob)
{
    return (u8)((((nudge - SEQ_NUDGE_MIN) & 0x07) << 4) |
                ((prob - 1) & 0x0F));
}

static s8 note_nudge(const seq_note_t *n)
{
    return (s8)(((n->extra >> 4) & 0x07) + SEQ_NUDGE_MIN);
}

static u8 note_prob(const seq_note_t *n)
{
    return (u8)((n->extra & 0x0F) + 1);
}

#define EXTRA_DEFAULT pack_extra(0, SEQ_PROB_MAX)

static seq_pattern_t *active_pat(u8 t)
{
    return &g_seq.track[t].pat[g_seq.track[t].active_pat];
}

static seq_pattern_t *edit_pat(u8 t)
{
    return &g_seq.track[t].pat[g_seq.track[t].edit_pat];
}

static seq_note_t *find_note(seq_pattern_t *pat, u8 step, u8 note)
{
    for (u8 n = 0; n < SEQ_NOTES_PER_STEP; ++n)
    {
        if (pat->step[step][n].note == note)
        {
            return &pat->step[step][n];
        }
    }
    return 0;
}

static u8 rand_mod(u8 mod)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return (u8)(s_rng % mod);
}

static void send_midi_out(u8 status, u8 d1, u8 d2)
{
#if SEQ_SEND_USB
    hal_send_midi(SEQ_USB_PORT, status, d1, d2);
#endif
#if SEQ_SEND_DIN
    hal_send_midi(DINMIDI, status, d1, d2);
#endif
}

static void slot_off(u8 t, u8 n)
{
    if (s_sounding_note[t][n] != SEQ_NOTE_EMPTY)
    {
        send_midi_out(NOTEOFF | s_sounding_ch[t][n],
                      s_sounding_note[t][n], 0);
        s_sounding_note[t][n] = SEQ_NOTE_EMPTY;
    }
    s_gate_left[t][n] = 0;
}

static void track_notes_off(u8 t)
{
    for (u8 n = 0; n < SEQ_NOTES_PER_STEP; ++n)
    {
        slot_off(t, n);
    }
}

static void all_notes_off(void)
{
    for (u8 t = 0; t < SEQ_TRACK_COUNT; ++t)
    {
        track_notes_off(t);
    }
}

static u8 any_solo(void)
{
    for (u8 t = 0; t < SEQ_TRACK_COUNT; ++t)
    {
        if (g_seq.track[t].solo)
        {
            return 1;
        }
    }
    return 0;
}

// a note's effective timing shift: its nudge plus global swing on
// off-steps, kept inside the step
static s8 note_shift(u8 t, const seq_note_t *n, u8 step)
{
    u8 rate = g_seq.track[t].rate;
    s16 shift = note_nudge(n);

    if (step & 1)
    {
        shift += (s16)((u16)s_swing * rate / 12);
    }

    if (shift >= rate)
    {
        shift = rate - 1;
    }
    if (shift <= -(s16)rate)
    {
        shift = -(s16)(rate - 1);
    }
    return (s8)shift;
}

// sound one note of a step (probability rolled here)
static void fire_note(u8 t, const seq_note_t *slot)
{
    seq_track_t *track = &g_seq.track[t];

    if (track->muted || (any_solo() && !track->solo))
    {
        return;
    }

    u8 prob = note_prob(slot);

    if (prob < SEQ_PROB_MAX && rand_mod(SEQ_PROB_MAX) >= prob)
    {
        return;
    }

    // find a sounding slot: replace the same note, else a free one,
    // else cut everything and start over
    u8 place = 0xFF;

    for (u8 n = 0; n < SEQ_NOTES_PER_STEP; ++n)
    {
        if (s_sounding_note[t][n] == slot->note)
        {
            slot_off(t, n);
            place = n;
            break;
        }
    }
    if (place == 0xFF)
    {
        for (u8 n = 0; n < SEQ_NOTES_PER_STEP; ++n)
        {
            if (s_sounding_note[t][n] == SEQ_NOTE_EMPTY)
            {
                place = n;
                break;
            }
        }
    }
    if (place == 0xFF)
    {
        track_notes_off(t);
        place = 0;
    }

    send_midi_out(NOTEON | track->channel, slot->note, slot->velocity);
    s_sounding_note[t][place] = slot->note;
    s_sounding_ch[t][place] = track->channel;

    // each note carries its own gate, so a late-fired neighbour can't
    // stretch an earlier note's length
    u8 gate = (u8)((u16)track->rate * track->gate / 4);

    s_gate_left[t][place] = gate ? gate : 1;
}

// chain helpers: the next pattern with its chain bit set
static u8 next_chained(u8 t, u8 from)
{
    u8 mask = g_seq.track[t].chain_mask;

    for (u8 i = 1; i <= SEQ_PATTERN_COUNT; ++i)
    {
        u8 p = (u8)((from + i) % SEQ_PATTERN_COUNT);

        if (mask & (1 << p))
        {
            return p;
        }
    }
    return from;
}

static u8 last_chained(u8 t)
{
    u8 mask = g_seq.track[t].chain_mask;

    for (s8 p = SEQ_PATTERN_COUNT - 1; p >= 0; --p)
    {
        if (mask & (1 << p))
        {
            return (u8)p;
        }
    }
    return 0;
}

// per-tick scan: notes of the current step fire at their non-negative
// shift; early-shifted notes of the NEXT step (possibly in the next
// chained pattern) fire late in this one
static void scan_track_notes(u8 t)
{
    seq_track_t *track = &g_seq.track[t];
    const seq_pattern_t *pat = active_pat(t);
    u8 rate = track->rate;
    u8 elapsed = s_tick[t];
    u8 s = s_pos[t];

    for (u8 n = 0; n < SEQ_NOTES_PER_STEP; ++n)
    {
        const seq_note_t *slot = &pat->step[s][n];

        if (slot->note == SEQ_NOTE_EMPTY)
        {
            continue;
        }

        s8 shift = note_shift(t, slot, s);

        if (shift >= 0 && (u8)shift == elapsed)
        {
            fire_note(t, slot);
            s_last_step[t] = s;
            s_last_pat[t] = track->active_pat;
        }
        else if (shift < 0 && elapsed == 0 && s_catchup[t])
        {
            // first boundary after start/seek: this note's early window
            // lay before time began - play it on the grid instead
            fire_note(t, slot);
            s_last_step[t] = s;
            s_last_pat[t] = track->active_pat;
        }
    }

    if (elapsed == 0)
    {
        s_catchup[t] = 0;
    }

    u8 s2 = (u8)(s + 1);
    const seq_pattern_t *pat2 = pat;

    if (s2 >= pat->length)
    {
        s2 = 0;
        pat2 = &track->pat[next_chained(t, track->active_pat)];
    }

    for (u8 n = 0; n < SEQ_NOTES_PER_STEP; ++n)
    {
        const seq_note_t *slot = &pat2->step[s2][n];

        if (slot->note == SEQ_NOTE_EMPTY)
        {
            continue;
        }

        s8 shift = note_shift(t, slot, s2);

        if (shift < 0 && (u8)(rate + shift) == elapsed)
        {
            fire_note(t, slot);
        }
    }
}

static void fill_retrigger(void)
{
    for (u8 t = 0; t < SEQ_TRACK_COUNT; ++t)
    {
        if (s_last_step[t] == NO_STEP)
        {
            continue;
        }

        // repeat the material from the pattern it was actually played
        // in, even if the chain has advanced meanwhile
        const seq_pattern_t *pat = &g_seq.track[t].pat[s_last_pat[t]];

        for (u8 n = 0; n < SEQ_NOTES_PER_STEP; ++n)
        {
            const seq_note_t *slot = &pat->step[s_last_step[t]][n];

            if (slot->note != SEQ_NOTE_EMPTY)
            {
                fire_note(t, slot);
            }
        }
    }
}

static void init_pattern(seq_pattern_t *pat)
{
    for (u8 s = 0; s < SEQ_STEP_COUNT; ++s)
    {
        for (u8 n = 0; n < SEQ_NOTES_PER_STEP; ++n)
        {
            pat->step[s][n].note = SEQ_NOTE_EMPTY;
            pat->step[s][n].velocity = 0;
            pat->step[s][n].extra = EXTRA_DEFAULT;
        }
    }
    pat->length = SEQ_STEP_COUNT;
}

void seq_engine_init(void)
{
    for (u8 t = 0; t < SEQ_TRACK_COUNT; ++t)
    {
        seq_track_t *track = &g_seq.track[t];

        for (u8 p = 0; p < SEQ_PATTERN_COUNT; ++p)
        {
            init_pattern(&track->pat[p]);
        }
        track->chain_mask = 1;
        track->active_pat = 0;
        track->edit_pat = 0;

        for (u8 i = 0; i < 16; ++i)
        {
            track->note_used[i] = 0;
        }
        track->channel = SEQ_DEFAULT_CHANNEL;
        track->muted = 0;
        track->solo = 0;
        track->cursor_note = DEFAULT_NOTES[t];
        track->drum_bank = 0;
        track->rate = SEQ_DEFAULT_RATE;
        track->gate = SEQ_DEFAULT_GATE;

        for (u8 n = 0; n < SEQ_NOTES_PER_STEP; ++n)
        {
            s_sounding_note[t][n] = SEQ_NOTE_EMPTY;
            s_gate_left[t][n] = 0;
        }
        s_last_step[t] = NO_STEP;
        s_last_pat[t] = 0;
        s_wraps[t] = 0;
        s_catchup[t] = 0;
        s_pos[t] = 0;
        s_tick[t] = 0;
    }

    s_fill_rate = 0;
    s_fill_held = 0;
    s_bar_tick = 0;
    s_swing = 0;

    g_seq.playing = 0;
}

void seq_engine_start(void)
{
    // park every playhead one step back - on the last chained pattern's
    // final step - so the first clock tick advances everything to the
    // first chained pattern's step 0, keeping the downbeat on that tick
    for (u8 t = 0; t < SEQ_TRACK_COUNT; ++t)
    {
        u8 last = last_chained(t);

        g_seq.track[t].active_pat = last;
        s_pos[t] = g_seq.track[t].pat[last].length - 1;
        s_tick[t] = 0;
        s_last_step[t] = NO_STEP;   // fills must not repeat stale material
        s_catchup[t] = 1;           // first boundary plays early-shifted notes
    }
    s_bar_tick = 0;
    s_fill_rate = 0;
    s_fill_held = 0;
    g_seq.playing = 1;
}

void seq_engine_pause(void)
{
    g_seq.playing = 0;
    all_notes_off();
    s_fill_rate = 0;
    s_fill_held = 0;
}

void seq_engine_resume(void)
{
    g_seq.playing = 1;
}

void seq_engine_stop(void)
{
    seq_engine_pause();

    for (u8 t = 0; t < SEQ_TRACK_COUNT; ++t)
    {
        g_seq.track[t].active_pat = next_chained(t, last_chained(t));
        s_pos[t] = 0;
        s_tick[t] = 0;
        s_last_step[t] = NO_STEP;
        s_catchup[t] = 0;
    }
    s_bar_tick = 0;
}

void seq_engine_seek(u32 tick)
{
    all_notes_off();

    for (u8 t = 0; t < SEQ_TRACK_COUNT; ++t)
    {
        seq_track_t *track = &g_seq.track[t];
        u8 rate = track->rate;

        // total ticks in one pass of the chain
        u32 span = 0;

        for (u8 p = 0; p < SEQ_PATTERN_COUNT; ++p)
        {
            if (track->chain_mask & (1 << p))
            {
                span += (u32)track->pat[p].length * rate;
            }
        }

        u32 local = tick % span;

        // walk the chained patterns to the one containing 'local'
        u8 pat = next_chained(t, last_chained(t));   // first chained

        while (local >= (u32)track->pat[pat].length * rate)
        {
            local -= (u32)track->pat[pat].length * rate;
            pat = next_chained(t, pat);
        }

        u8 stepidx = (u8)(local / rate);
        u8 intick = (u8)(local % rate);

        if (intick == 0)
        {
            // exactly on a boundary: park one step back so the next
            // incoming tick triggers this very step
            if (stepidx == 0)
            {
                u8 prev = last_chained(t);
                u8 scan = next_chained(t, last_chained(t));

                while (scan != pat)                 // pattern before 'pat'
                {
                    prev = scan;
                    scan = next_chained(t, scan);
                }

                track->active_pat = prev;
                s_pos[t] = track->pat[prev].length - 1;
            }
            else
            {
                track->active_pat = pat;
                s_pos[t] = stepidx - 1;
            }
            s_tick[t] = 0;
            s_catchup[t] = 1;   // the landed-on step's early notes still play
        }
        else
        {
            track->active_pat = pat;
            s_pos[t] = stepidx;
            s_tick[t] = intick;
            s_catchup[t] = 0;
        }
        s_last_step[t] = NO_STEP;
    }

    s_bar_tick = (u8)(tick % SEQ_BAR_TICKS);
}

void seq_engine_clock_tick(void)
{
    if (!g_seq.playing)
    {
        return;
    }

    // gates first, so a full-length gate ends before the next trigger;
    // each sounding slot counts down on its own
    for (u8 t = 0; t < SEQ_TRACK_COUNT; ++t)
    {
        for (u8 n = 0; n < SEQ_NOTES_PER_STEP; ++n)
        {
            if (s_gate_left[t][n] && --s_gate_left[t][n] == 0)
            {
                slot_off(t, n);
            }
        }
    }

    // fills end cleanly on the "1" unless the button is still held
    if (s_fill_rate && !s_fill_held && s_bar_tick == 0)
    {
        s_fill_rate = 0;
    }

    // advance each track's own playhead and fire the notes due this tick
    for (u8 t = 0; t < SEQ_TRACK_COUNT; ++t)
    {
        seq_track_t *track = &g_seq.track[t];

        if (s_tick[t] == 0)
        {
            u8 next = (u8)(s_pos[t] + 1);

            if (next >= active_pat(t)->length)
            {
                next = 0;
                track->active_pat = next_chained(t, track->active_pat);
                ++s_wraps[t];       // pattern end passed (capture disarm)
            }
            s_pos[t] = next;
        }

        if (!s_fill_rate)
        {
            scan_track_notes(t);
        }

        if (++s_tick[t] >= track->rate)
        {
            s_tick[t] = 0;
        }
    }

    if (s_fill_rate)
    {
        if (s_fill_phase == 0)
        {
            fill_retrigger();
        }
        if (++s_fill_phase >= s_fill_rate)
        {
            s_fill_phase = 0;
        }
    }

    if (++s_bar_tick >= SEQ_BAR_TICKS)
    {
        s_bar_tick = 0;
    }
}

void seq_engine_fill(u8 rate_ticks, u8 held)
{
    if (held)
    {
        if (!g_seq.playing || !rate_ticks)
        {
            return;
        }

        u8 was_active = s_fill_rate != 0;

        s_fill_rate = rate_ticks;
        s_fill_held = 1;

        if (!was_active)
        {
            fill_retrigger();       // respond on the press itself
            s_fill_phase = 1;
        }
        else if (s_fill_phase >= s_fill_rate)
        {
            s_fill_phase = 0;       // rate switched to a faster one
        }
    }
    else
    {
        s_fill_held = 0;            // latched until the next bar line

        if (!g_seq.playing)
        {
            s_fill_rate = 0;
        }
    }
}

u8 seq_engine_fill_rate(void)
{
    return s_fill_rate;
}

u8 seq_engine_bar_tick(void)
{
    return s_bar_tick;
}

void seq_engine_send_note(u8 channel, u8 note, u8 velocity)
{
    if (velocity)
    {
        send_midi_out(NOTEON | channel, note, velocity);
    }
    else
    {
        send_midi_out(NOTEOFF | channel, note, 0);
    }
}

void seq_engine_send_aftertouch(u8 channel, u8 note, u8 pressure)
{
    send_midi_out(POLYAFTERTOUCH | channel, note, pressure);
}

static void set_note_used(u8 track, u8 note, u8 used)
{
    if (used)
    {
        g_seq.track[track].note_used[note >> 3] |= (u8)(1 << (note & 7));
    }
    else
    {
        g_seq.track[track].note_used[note >> 3] &= (u8)~(1 << (note & 7));
    }
}

u8 seq_engine_note_used(u8 track, u8 note)
{
    return g_seq.track[track].note_used[note >> 3] & (1 << (note & 7));
}

// is the note placed anywhere in any pattern of the track?
static u8 note_anywhere(u8 track, u8 note)
{
    for (u8 p = 0; p < SEQ_PATTERN_COUNT; ++p)
    {
        const seq_pattern_t *pat = &g_seq.track[track].pat[p];

        for (u8 s = 0; s < SEQ_STEP_COUNT; ++s)
        {
            for (u8 n = 0; n < SEQ_NOTES_PER_STEP; ++n)
            {
                if (pat->step[s][n].note == note)
                {
                    return 1;
                }
            }
        }
    }
    return 0;
}

u8 seq_engine_note_velocity(u8 track, u8 step, u8 note)
{
    const seq_note_t *slot = find_note(edit_pat(track), step, note);

    return slot ? slot->velocity : 0;
}

void seq_engine_toggle_note(u8 track, u8 step, u8 note, u8 velocity)
{
    if (track >= SEQ_TRACK_COUNT || step >= SEQ_STEP_COUNT || note > 127)
    {
        return;
    }

    seq_pattern_t *pat = edit_pat(track);
    seq_note_t *slot = find_note(pat, step, note);

    if (slot)                       // present? remove it
    {
        slot->note = SEQ_NOTE_EMPTY;
        slot->velocity = 0;
        slot->extra = EXTRA_DEFAULT;

        if (!note_anywhere(track, note))
        {
            set_note_used(track, note, 0);
        }
        return;
    }

    slot = find_note(pat, step, SEQ_NOTE_EMPTY);

    if (slot)                       // add to a free slot
    {
        slot->note = note;
        slot->velocity = velocity ? velocity : 1;
        slot->extra = EXTRA_DEFAULT;
        set_note_used(track, note, 1);
    }
}

void seq_engine_set_note(u8 track, u8 step, u8 note, u8 velocity)
{
    if (track >= SEQ_TRACK_COUNT || step >= SEQ_STEP_COUNT || note > 127)
    {
        return;
    }

    if (!velocity)
    {
        velocity = 1;   // callers choose the floor; zero would mean "off"
    }

    seq_pattern_t *pat = edit_pat(track);
    seq_note_t *slot = find_note(pat, step, note);

    if (slot)
    {
        slot->velocity = velocity;
        return;
    }

    slot = find_note(pat, step, SEQ_NOTE_EMPTY);

    if (slot)
    {
        slot->note = note;
        slot->velocity = velocity;
        slot->extra = EXTRA_DEFAULT;
        set_note_used(track, note, 1);
    }
}

void seq_engine_clear_lane(u8 track, u8 note)
{
    if (track >= SEQ_TRACK_COUNT || note > 127)
    {
        return;
    }

    seq_pattern_t *pat = edit_pat(track);

    for (u8 s = 0; s < SEQ_STEP_COUNT; ++s)
    {
        seq_note_t *slot = find_note(pat, s, note);

        if (slot)
        {
            slot->note = SEQ_NOTE_EMPTY;
            slot->velocity = 0;
            slot->extra = EXTRA_DEFAULT;
        }
    }

    if (!note_anywhere(track, note))
    {
        set_note_used(track, note, 0);
    }
}

void seq_engine_nudge_step(u8 track, u8 step, u8 note, s8 delta)
{
    seq_engine_set_nudge(track, step, note,
                         (s8)(seq_engine_step_nudge(track, step, note)
                              + delta));
}

void seq_engine_set_nudge(u8 track, u8 step, u8 note, s8 nudge)
{
    if (track >= SEQ_TRACK_COUNT || step >= SEQ_STEP_COUNT)
    {
        return;
    }

    seq_note_t *slot = find_note(edit_pat(track), step, note);

    if (!slot)
    {
        return;
    }

    if (nudge < SEQ_NUDGE_MIN)
    {
        nudge = SEQ_NUDGE_MIN;
    }
    if (nudge > SEQ_NUDGE_MAX)
    {
        nudge = SEQ_NUDGE_MAX;
    }
    slot->extra = pack_extra(nudge, note_prob(slot));
}

s8 seq_engine_step_nudge(u8 track, u8 step, u8 note)
{
    const seq_note_t *slot = find_note(edit_pat(track), step, note);

    return slot ? note_nudge(slot) : 0;
}

void seq_engine_set_prob(u8 track, u8 step, u8 note, u8 prob)
{
    if (track >= SEQ_TRACK_COUNT || step >= SEQ_STEP_COUNT)
    {
        return;
    }

    seq_note_t *slot = find_note(edit_pat(track), step, note);

    if (!slot)
    {
        return;
    }

    if (prob < 1)
    {
        prob = 1;
    }
    if (prob > SEQ_PROB_MAX)
    {
        prob = SEQ_PROB_MAX;
    }
    slot->extra = pack_extra(note_nudge(slot), prob);
}

u8 seq_engine_step_prob(u8 track, u8 step, u8 note)
{
    const seq_note_t *slot = find_note(edit_pat(track), step, note);

    return slot ? note_prob(slot) : SEQ_PROB_MAX;
}

u8 seq_engine_track_pos(u8 track)
{
    return s_pos[track];
}

u8 seq_engine_track_wraps(u8 track)
{
    return s_wraps[track];
}

u8 seq_engine_length(u8 track)
{
    return edit_pat(track)->length;
}

void seq_engine_set_length(u8 track, u8 length)
{
    if (track >= SEQ_TRACK_COUNT || length < 1 || length > SEQ_STEP_COUNT)
    {
        return;
    }
    // a playhead beyond the new length falls back in range on its
    // next advance (the wrap check handles it)
    edit_pat(track)->length = length;
}

void seq_engine_set_rate(u8 track, u8 rate_ticks)
{
    if (track >= SEQ_TRACK_COUNT)
    {
        return;
    }

    for (u8 i = 0; i < SEQ_RATE_COUNT; ++i)
    {
        if (SEQ_RATES[i] == rate_ticks)
        {
            g_seq.track[track].rate = rate_ticks;
            return;
        }
    }
}

void seq_engine_select_edit(u8 track, u8 pattern)
{
    if (track < SEQ_TRACK_COUNT && pattern < SEQ_PATTERN_COUNT)
    {
        g_seq.track[track].edit_pat = pattern;
    }
}

// pattern clipboard (a full snapshot, so later edits to the source don't
// change what gets pasted)
static seq_pattern_t s_clipboard;
static u8 s_clip_full;

void seq_engine_copy_pattern(u8 track)
{
    if (track < SEQ_TRACK_COUNT)
    {
        s_clipboard = *edit_pat(track);
        s_clip_full = 1;
    }
}

void seq_engine_paste_pattern(u8 track)
{
    if (track < SEQ_TRACK_COUNT && s_clip_full)
    {
        *edit_pat(track) = s_clipboard;
        seq_engine_rebuild_note_usage();
    }
}

u8 seq_engine_clipboard_full(void)
{
    return s_clip_full;
}

void seq_engine_toggle_chain(u8 track, u8 pattern)
{
    if (track >= SEQ_TRACK_COUNT || pattern >= SEQ_PATTERN_COUNT)
    {
        return;
    }

    u8 mask = g_seq.track[track].chain_mask ^ (u8)(1 << pattern);

    if (mask)                       // at least one pattern stays chained
    {
        g_seq.track[track].chain_mask = mask;
    }
}

void seq_engine_set_swing(u8 swing)
{
    s_swing = (swing > SEQ_SWING_MAX) ? SEQ_SWING_MAX : swing;
}

u8 seq_engine_swing(void)
{
    return s_swing;
}

void seq_engine_capture_pos(u8 track, u8 quant_ticks,
                            u8 *pattern, u8 *step, u8 *offset)
{
    const seq_track_t *tr = &g_seq.track[track];
    const seq_pattern_t *pat = active_pat(track);
    u8 rate = tr->rate;
    u16 span = (u16)pat->length * rate;

    *pattern = tr->active_pat;

    // ticks elapsed since this track's current step triggered
    u8 elapsed = (u8)((s_tick[track] + rate - 1) % rate);
    u16 total = (u16)s_pos[track] * rate + elapsed;

    // the quantize grid only applies where it divides evenly into the
    // track's own grid (or vice versa); otherwise fall back to the step
    u16 q = quant_ticks;

    if (q < rate)
    {
        if (rate % q)
        {
            q = rate;
        }
    }
    else if (q > rate && (q % rate))
    {
        q = rate;
    }

    u16 g = (u16)(((total + q / 2) / q) * q);
    u8 s;
    u8 off;

    if (g >= span)
    {
        // rounded forward past the pattern end: the hit belongs to the
        // start of the NEXT chained pattern, not back at our step 0
        *pattern = next_chained(track, tr->active_pat);
        s = 0;
        off = (u8)(g - span);

        if (off > SEQ_NUDGE_MAX)
        {
            off = 0;
        }
        *step = s;
        *offset = off;
        return;
    }

    s = (u8)(g / rate);
    off = (u8)(g % rate);

    // sub-step offsets are stored as the note's nudge; unrepresentable
    // ones round to the nearest whole step instead
    if (off > SEQ_NUDGE_MAX)
    {
        g = (u16)(((total + rate / 2) / rate) * rate);

        if (g >= span)
        {
            *pattern = next_chained(track, tr->active_pat);
            s = 0;
        }
        else
        {
            s = (u8)(g / rate);
        }
        off = 0;
    }

    *step = s;
    *offset = off;
}

void seq_engine_capture_write(u8 track, u8 pattern, u8 step, u8 note,
                              u8 velocity, u8 offset)
{
    if (track >= SEQ_TRACK_COUNT || pattern >= SEQ_PATTERN_COUNT ||
        step >= SEQ_STEP_COUNT || note > 127)
    {
        return;
    }

    if (!velocity)
    {
        velocity = 1;
    }

    seq_pattern_t *pat = &g_seq.track[track].pat[pattern];
    seq_note_t *slot = find_note(pat, step, note);

    if (!slot)
    {
        slot = find_note(pat, step, SEQ_NOTE_EMPTY);

        if (!slot)
        {
            return;                 // step full
        }
        slot->note = note;
        slot->extra = EXTRA_DEFAULT;
        set_note_used(track, note, 1);
    }

    slot->velocity = velocity;

    if (offset && offset <= SEQ_NUDGE_MAX)
    {
        slot->extra = pack_extra((s8)offset, note_prob(slot));
    }
}

void seq_engine_rebuild_note_usage(void)
{
    for (u8 t = 0; t < SEQ_TRACK_COUNT; ++t)
    {
        seq_track_t *track = &g_seq.track[t];

        for (u8 i = 0; i < 16; ++i)
        {
            track->note_used[i] = 0;
        }

        for (u8 p = 0; p < SEQ_PATTERN_COUNT; ++p)
        {
            for (u8 s = 0; s < SEQ_STEP_COUNT; ++s)
            {
                for (u8 n = 0; n < SEQ_NOTES_PER_STEP; ++n)
                {
                    if (track->pat[p].step[s][n].note != SEQ_NOTE_EMPTY)
                    {
                        set_note_used(t, track->pat[p].step[s][n].note, 1);
                    }
                }
            }
        }
    }
}
