#ifndef SEQ_CLOCK_H
#define SEQ_CLOCK_H

#include "app_defs.h"

// ____________________________________________________________________________
//
// Clock and transport. Two modes, switched automatically:
//   - internal: the 1ms timer is divided down to 24 PPQN MIDI clock, which is
//     sent to the outputs and drives the engine
//   - external: whenever MIDI clock pulses are arriving (no START required),
//     they own the timing - the internal clock is halted and the engine
//     advances only on incoming pulses, so it can never drift against the
//     master. Pulses are counted even while stopped, so the master's bar
//     phase is always known. If the pulses stop for
//     SEQ_EXT_CLOCK_TIMEOUT_MS, the internal clock takes over seamlessly.
//
// Transport semantics follow MIDI: START restarts from the top, STOP
// pauses in place, CONTINUE resumes - honouring a received Song Position
// Pointer.
// ____________________________________________________________________________

void seq_clock_init(void);

// halt for a total reset: sends MIDI STOP if we were driving the
// transport, cancels a pending bar-quantized start. Call BEFORE stopping
// the engine (it checks g_seq.playing).
void seq_clock_halt(void);

// call once per app_timer_event (1ms)
void seq_clock_ms(void);

// play/stop button. Internally clocked: toggles immediately and sends
// MIDI START/STOP. Slaved: starting arms a bar-quantized start (the
// pattern begins on the master's next "1"; press again to cancel),
// stopping is immediate; no transport is sent while slaved.
void seq_clock_toggle_play(void);

void seq_clock_set_bpm(u16 bpm);
u16 seq_clock_get_bpm(void);

// nonzero while an external MIDI clock is present (pulse within timeout)
u8 seq_clock_is_external(void);

// nonzero while a bar-quantized start is pending
u8 seq_clock_is_armed(void);

// feed MIDI input: realtime (0xF8/0xFA/0xFB/0xFC) and song position (0xF2)
void seq_clock_midi_in(u8 status, u8 d1, u8 d2);

#endif
