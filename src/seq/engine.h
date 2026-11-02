#ifndef SEQ_ENGINE_H
#define SEQ_ENGINE_H

#include "app_defs.h"
#include "config.h"

// ____________________________________________________________________________
//
// The sequencer engine: pattern data model plus note on/off scheduling.
// It is driven exclusively by MIDI clock ticks (24 PPQN), whether those come
// from the internal clock or from an external MIDI clock source, so timing
// stays tempo-relative.
//
// Tracks are polyphonic (up to SEQ_NOTES_PER_STEP notes per step) and run
// their own playhead: each track has its own step rate and each pattern its
// own length (polymeter). A track holds SEQ_PATTERN_COUNT patterns; the
// chain_mask selects which of them the track cycles through, advancing at
// each pattern end.
//
// The UI shows one "note lane" at a time (track->cursor_note) - the Mk3
// workflow: pick a note, see and edit only that note's steps.
// ____________________________________________________________________________

typedef struct
{
    u8 note;                        // SEQ_NOTE_EMPTY = unused slot
    u8 velocity;
    u8 extra;                       // packed per-note nudge (bits 4-6,
                                    // offset by SEQ_NUDGE_MIN) and
                                    // probability-1 (bits 0-3)
} seq_note_t;

typedef struct
{
    seq_note_t step[SEQ_STEP_COUNT][SEQ_NOTES_PER_STEP];
    u8 length;                      // steps in this pattern's loop, 1..32
} seq_pattern_t;

typedef struct
{
    seq_pattern_t pat[SEQ_PATTERN_COUNT];
    u8 chain_mask;                  // patterns in the loop (never 0)
    u8 active_pat;                  // pattern currently playing
    u8 edit_pat;                    // pattern currently viewed/edited
    u8 note_used[16];               // bitmap: notes placed anywhere in track
    u8 channel;                     // MIDI channel, 0..15
    u8 muted;
    u8 solo;                        // any solo set = only soloed tracks play
    u8 cursor_note;                 // note lane currently shown/edited
    s8 drum_bank;                   // this track's drum-pad bank shift
    u8 rate;                        // clock ticks per step (see SEQ_RATES)
    u8 gate;                        // gate length in quarter-steps, 1..4
} seq_track_t;

typedef struct
{
    seq_track_t track[SEQ_TRACK_COUNT];
    u8 playing;
} seq_engine_t;

extern seq_engine_t g_seq;

// the selectable step rates, in clock ticks
extern const u8 SEQ_RATES[SEQ_RATE_COUNT];

void seq_engine_init(void);

// transport: start restarts from the top; pause freezes in place and
// resume continues from there (MIDI STOP/CONTINUE semantics)
void seq_engine_start(void);
void seq_engine_stop(void);         // stop and rewind to the top
void seq_engine_pause(void);
void seq_engine_resume(void);

// jump to an absolute position, in clock ticks since the song start
// (from a MIDI Song Position Pointer; 1 SPP unit = 6 ticks)
void seq_engine_seek(u32 tick);

// advance gates / playheads; call once per MIDI clock tick while playing
void seq_engine_clock_tick(void);

// all editing below applies to the track's EDIT pattern (edit_pat) -
// playback independently follows the chain. Only capture writes into the
// playing pattern (see seq_engine_capture_write).

// choose which pattern the track shows and edits
void seq_engine_select_edit(u8 track, u8 pattern);

// pattern clipboard: copy snapshots the track's edit pattern; paste
// overwrites the (possibly different) track's edit pattern with it
void seq_engine_copy_pattern(u8 track);
void seq_engine_paste_pattern(u8 track);
u8 seq_engine_clipboard_full(void);

// add the note to a step (storing the velocity) or remove it.
// Silently ignored when the step's slots are all full.
void seq_engine_toggle_note(u8 track, u8 step, u8 note, u8 velocity);

// add or update a note at a step - the capture/record path, never removes
void seq_engine_set_note(u8 track, u8 step, u8 note, u8 velocity);

// the note's velocity at a step, 0 if not placed there
u8 seq_engine_note_velocity(u8 track, u8 step, u8 note);

// nonzero if the note is placed on any step of any pattern of the track
u8 seq_engine_note_used(u8 track, u8 note);

// remove a note from every step of the EDIT (viewed) pattern
void seq_engine_clear_lane(u8 track, u8 note);

// per-NOTE micro-timing, addressed by (step, note); no-ops when the note
// isn't placed at that step. Clamped to [SEQ_NUDGE_MIN, SEQ_NUDGE_MAX].
void seq_engine_nudge_step(u8 track, u8 step, u8 note, s8 delta);
void seq_engine_set_nudge(u8 track, u8 step, u8 note, s8 nudge);
s8 seq_engine_step_nudge(u8 track, u8 step, u8 note);   // 0 if absent

// per-NOTE probability, 1..SEQ_PROB_MAX
void seq_engine_set_prob(u8 track, u8 step, u8 note, u8 prob);
u8 seq_engine_step_prob(u8 track, u8 step, u8 note);    // MAX if absent

// each track runs its own playhead (polymeter): current position, and a
// free-running count of pattern ends passed (for capture auto-disarm)
u8 seq_engine_track_pos(u8 track);
u8 seq_engine_track_wraps(u8 track);

// EDIT (viewed) pattern's loop length (1..32) and the track's step rate
u8 seq_engine_length(u8 track);
void seq_engine_set_length(u8 track, u8 length);
void seq_engine_set_rate(u8 track, u8 rate_ticks);

// toggle a pattern in/out of the track's chain (at least one stays)
void seq_engine_toggle_chain(u8 track, u8 pattern);

// global swing, 0..SEQ_SWING_MAX
void seq_engine_set_swing(u8 swing);
u8 seq_engine_swing(void);

// where a note captured right now should land on the track's own grid:
// the pattern (the playing one, or the next chained one when the hit
// rounds forward past the pattern end), the step, plus a tick offset
// within it (0 when the quantize grid is at or above step resolution,
// or when the offset can't be stored as nudge)
void seq_engine_capture_pos(u8 track, u8 quant_ticks,
                            u8 *pattern, u8 *step, u8 *offset);

// write a captured note (and its nudge offset) into that pattern
void seq_engine_capture_write(u8 track, u8 pattern, u8 step, u8 note,
                              u8 velocity, u8 offset);

// recompute the note_used bitmaps (after bulk-loading pattern data)
void seq_engine_rebuild_note_usage(void);

// beat repeat / fill: while active, tracks retrigger their most recent
// step at rate_ticks intervals instead of playing the pattern; it always
// ends on a bar line - released early, it stays latched until the next
// "1", and held across it, it continues. rate 0 is never passed.
void seq_engine_fill(u8 rate_ticks, u8 held);

// the active fill rate in ticks, 0 when no fill is running
u8 seq_engine_fill_rate(void);

// position within the bar, 0..SEQ_BAR_TICKS-1 (0 = the "1"; for the
// metronome display)
u8 seq_engine_bar_tick(void);

// send a live (non-sequenced) note to the outputs; velocity 0 = note off
void seq_engine_send_note(u8 channel, u8 note, u8 velocity);

// send polyphonic aftertouch for a live note
void seq_engine_send_aftertouch(u8 channel, u8 note, u8 pressure);

#endif
