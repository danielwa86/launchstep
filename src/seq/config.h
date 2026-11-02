#ifndef SEQ_CONFIG_H
#define SEQ_CONFIG_H

#include "app_defs.h"

// ____________________________________________________________________________
//
// LaunchStep - central configuration.
//
// Everything a tinkerer might want to tweak first lives here: track/step
// counts, clock resolution, default tempo and the MIDI output routing.
// ____________________________________________________________________________

#define SEQ_TRACK_COUNT     4
#define SEQ_STEP_COUNT      32

// polyphony: how many notes one step can hold. 6 (not the Mk3's 8) so
// that per-note nudge/probability fits the Mk1's 20K RAM
#define SEQ_NOTES_PER_STEP  6
#define SEQ_NOTE_EMPTY      0xFF

#define SEQ_DEFAULT_BPM     120
#define SEQ_MIN_BPM         40
#define SEQ_MAX_BPM         240
#define SEQ_BPM_INCREMENT   5

// external MIDI clock counts as present until this long without a pulse
// (500ms = 8 missed pulses at the slowest supported tempo)
#define SEQ_EXT_CLOCK_TIMEOUT_MS 500

// MIDI beat clock is 24 pulses per quarter note; steps are 16th notes.
#define SEQ_PPQN            24
#define SEQ_TICKS_PER_STEP  (SEQ_PPQN / 4)

// patterns per track (chained in the hold-track overlay)
#define SEQ_PATTERN_COUNT   4

// step probability: 1..SEQ_PROB_MAX chances out of SEQ_PROB_MAX
#define SEQ_PROB_MAX        16

// default gate length, in quarters of a step (2 = 50%)
#define SEQ_DEFAULT_GATE    2

// global swing 0..SEQ_SWING_MAX, delaying every off-step; at 16th rate,
// SEQ_SWING_MAX is a full triplet feel
#define SEQ_SWING_MAX       6

#define SEQ_STEPS_PER_BAR   16
#define SEQ_PATTERN_TICKS   (SEQ_STEP_COUNT * SEQ_TICKS_PER_STEP)
#define SEQ_BAR_TICKS       (SEQ_STEPS_PER_BAR * SEQ_TICKS_PER_STEP)

// per-track step rates (clock ticks per step at 24 PPQN):
// 32nd, 16th triplet, 16th, 8th triplet, 8th, 1/4 triplet, 1/4, 1/2
#define SEQ_RATE_COUNT      8
#define SEQ_DEFAULT_RATE    SEQ_TICKS_PER_STEP

// per-step micro-timing, in clock ticks (1 tick = 1/96 note; +3 = a 32nd)
#define SEQ_NUDGE_MIN       (-2)
#define SEQ_NUDGE_MAX       3

// capture quantize grids, in clock ticks
#define SEQ_QUANT_8TH       12
#define SEQ_QUANT_16TH      6
#define SEQ_QUANT_32ND      3

// pads can report very low velocities on a soft touch - clamp them up
#define SEQ_MIN_VELOCITY    15

// how long a step must be held before hold-editing (probability /
// velocity / nudge) engages - quick taps stay plain toggles
#define SEQ_HOLD_MS         180

// how long Delete must be held before the lane clear fires (destructive,
// so a stray tap must not trigger it)
#define SEQ_CLEAR_HOLD_MS   600

// mode buttons held at least this long act momentarily: the mode reverts
// on release. Shorter presses switch and stay.
#define SEQ_MODE_HOLD_MS    300

// default per-track output: GM drum notes on MIDI channel 10 (zero-based 9)
#define SEQ_DEFAULT_CHANNEL 9

// drum pads (lower-left 4x4): chromatic from DRUM_BASE_NOTE (C1, drum-rack
// convention), bottom-left pad first, running right then up. The bank
// buttons shift the whole 4x4 by DRUM_BANK_SIZE notes.
#define DRUM_BASE_NOTE      36
#define DRUM_BANK_SIZE      16
#define DRUM_BANK_MIN       (-2)
#define DRUM_BANK_MAX       4

// velocity selector (lower-right 4x4): 16 levels, (level+1)*8-1 = 7..127.
// New steps are entered with the selected level.
#define SEQ_DEFAULT_VELOCITY 103

// note mode (8x8 playing surface): chromatic rows stacked in fourths, or
// scale mode (pads = scale degrees, rows a 4th of degrees apart); its own
// MIDI channel, root, scale, transpose and octave, set on the settings
// page (hold the Note button while in note mode)
#define NOTE_MODE_BASE      36
#define NOTE_MODE_INTERVAL  5
#define NOTE_SCALE_INTERVAL 3
#define NOTE_OCT_MIN        (-2)
#define NOTE_OCT_MAX        4
#define NOTE_TRANSPOSE_MAX  12
#define SEQ_SCALE_COUNT     16

// bytes in the persisted note-mode setup blob
#define NOTE_SETUP_BYTES    6

// device mode: 8 column faders as CC, and user mode: 64 assignable pads
// as notes - both on their own control channel (zero-based 15 = channel 16)
// so they never collide with sequenced notes and are excluded from capture
#define SEQ_CTRL_CHANNEL    15
#define SEQ_FADER_CC_BASE   20
#define SEQ_FADER_UPDATE_MS 20

// where sequenced notes, clock and transport are sent.
// USBSTANDALONE is the USB port the factory firmware uses in standalone mode;
// set SEQ_SEND_USB/SEQ_SEND_DIN to 0 to disable either output.
#define SEQ_SEND_USB        1
#define SEQ_SEND_DIN        1
#define SEQ_USB_PORT        USBSTANDALONE

#endif
