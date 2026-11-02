#ifndef SEQ_UI_H
#define SEQ_UI_H

#include "app_defs.h"

// ____________________________________________________________________________
//
// UI: renders the surface (steps, playhead, track select, transport) and
// handles pad/button input. See layout.h for the physical arrangement.
// ____________________________________________________________________________

void seq_ui_init(void);

// call once per app_timer_event (1ms); event-driven - repaints only when
// state changed, sliced across ticks to keep the LED scan undisturbed
void seq_ui_ms(void);

// pad/button events (TYPEPAD)
void seq_ui_surface_event(u8 index, u8 value);

// the Setup button: save to flash; with Shift held ~0.6 s, total reset
void seq_ui_setup_event(u8 pressed);

// incoming channel MIDI (for capture mode); realtime goes to the clock
void seq_ui_midi_event(u8 status, u8 d1, u8 d2);

// pad pressure (for drum-pad aftertouch in dynamic velocity mode)
void seq_ui_aftertouch_event(u8 index, u8 value);

u8 seq_ui_selected_track(void);
void seq_ui_select_track(u8 track);

// entry-velocity selection (0 = dynamic mode), exposed for flash
// persistence; per-track state (bank, mute, solo) lives in seq_track_t
u8 seq_ui_get_velocity(void);
void seq_ui_set_velocity(u8 velocity);

// capture quantize grid in ticks (12/6/3), exposed for flash persistence
u8 seq_ui_get_quantize(void);
void seq_ui_set_quantize(u8 ticks);

// note-mode setup blob for persistence: root, scale, flags (bit0 = scale
// mode, bit1 = aftertouch), transpose (signed), channel, octave (signed)
void seq_ui_get_note_setup(u8 *out);
void seq_ui_set_note_setup(const u8 *in);

#endif
