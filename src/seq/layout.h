#ifndef SEQ_LAYOUT_H
#define SEQ_LAYOUT_H

#include "app_defs.h"
#include "config.h"

// ____________________________________________________________________________
//
// Physical layout of the control surface.
//
// Button indices are 10*row + col as documented in app.h:
//
//   (90) 91 92 93 94 95 96 97 98 (99)     <- top round buttons
//    80  81 82 83 84 85 86 87 88  89
//    ..  .. grid pads rows 8..1 ..  ..    <- right/left cols are round buttons
//    10  11 12 13 14 15 16 17 18  19
//    (0)  1  2  3  4  5  6  7  8  (9)     <- bottom round buttons
//
// LaunchStep layout (drum-programming focused):
//   - grid rows 8..5: the 32 steps of the selected track, 8 per row,
//     running left to right, top to bottom (XOX style)
//   - grid rows 4..1, columns 1..4: drum pads - chromatic from C1,
//     bottom-left first, right then up (drum-rack order). Playing one
//     selects that note lane and sends the note live
//   - grid rows 4..1, columns 5..8: velocity selector, 16 levels, same
//     ordering. Sets the entry velocity; while a step is held it edits
//     that step instead
//   - left round buttons 40/30 (beside the pads): drum bank up/down
//   - left round button 10 (physical "Record Arm"): capture mode
//   - left round button 50 (physical "Delete"): clear the shown note lane
//   - bottom round buttons 1..4: track select
//   - top round button 91: play/stop, 92/93: tempo -/+
// ____________________________________________________________________________

#define SEQ_STEP_TOP_ROW    8
#define SEQ_STEP_ROWS       (SEQ_STEP_COUNT / 8)

#define BTN_PLAY            91
#define BTN_TEMPO_DOWN      92
#define BTN_TEMPO_UP        93

// cycles capture quantize: 8th -> 16th -> 32nd
#define BTN_QUANTIZE        94

// mode select (physical Session / Note / Device / User buttons); pressing
// Note again while in note mode toggles aftertouch
#define BTN_SESSION         95
#define BTN_NOTE            96
#define BTN_DEVICE          97
#define BTN_USER            98

#define BTN_BANK_UP         40
#define BTN_BANK_DOWN       30

// physical "Record Arm": arm/disarm MIDI capture into the selected track
#define BTN_RECORD          10

// physical "Delete": clear the shown note lane from the selected track
#define BTN_CLEAR           50

// global swing down/up
#define BTN_SWING_DOWN      60
#define BTN_SWING_UP        70

// physical "Shift": held, drum pads select their lane without sounding
#define BTN_SHIFT           80

// physical "Double": paste the pattern clipboard; with Shift held, copy
// the viewed pattern into it
#define BTN_DOUBLE          20

// bottom round buttons 1..SEQ_TRACK_COUNT select the track
#define BTN_TRACK_BASE      1

// bottom round buttons 5..4+SEQ_TRACK_COUNT mute tracks 1..4
#define BTN_MUTE_BASE       5

#define SEQ_NO_STEP         0xFF
#define SEQ_NO_PAD          0xFF

#define DRUM_PAD_COUNT      16
#define VEL_PAD_COUNT       16

// any 8x8 grid pad as 0..63 (bottom-left first, right then up), or
// SEQ_NO_PAD for the round buttons
static inline u8 seq_pad_to_grid(u8 index)
{
    u8 row = index / 10;
    u8 col = index % 10;

    if (row < 1 || row > 8 || col < 1 || col > 8)
    {
        return SEQ_NO_PAD;
    }

    return (u8)((row - 1) * 8 + (col - 1));
}

static inline u8 seq_grid_to_pad(u8 grid)
{
    return (u8)(10 * (1 + grid / 8) + (1 + grid % 8));
}

static inline u8 seq_step_to_pad(u8 step)
{
    return (u8)(10 * (SEQ_STEP_TOP_ROW - step / 8) + (1 + step % 8));
}

// returns SEQ_NO_STEP if the pad is not part of the step area
static inline u8 seq_pad_to_step(u8 index)
{
    u8 row = index / 10;
    u8 col = index % 10;

    if (col < 1 || col > 8)
    {
        return SEQ_NO_STEP;
    }
    if (row > SEQ_STEP_TOP_ROW || row <= SEQ_STEP_TOP_ROW - SEQ_STEP_ROWS)
    {
        return SEQ_NO_STEP;
    }

    return (u8)((SEQ_STEP_TOP_ROW - row) * 8 + (col - 1));
}

// drum pads 0..15: pad/4 = row (0 = bottom grid row), pad%4 = column
static inline u8 seq_pad_to_drum(u8 index)
{
    u8 row = index / 10;
    u8 col = index % 10;

    if (row < 1 || row > 4 || col < 1 || col > 4)
    {
        return SEQ_NO_PAD;
    }

    return (u8)((row - 1) * 4 + (col - 1));
}

static inline u8 seq_drum_to_pad(u8 drum)
{
    return (u8)(10 * (1 + drum / 4) + (1 + drum % 4));
}

// velocity pads 0..15, same ordering in columns 5..8
static inline u8 seq_pad_to_vel(u8 index)
{
    u8 row = index / 10;
    u8 col = index % 10;

    if (row < 1 || row > 4 || col < 5 || col > 8)
    {
        return SEQ_NO_PAD;
    }

    return (u8)((row - 1) * 4 + (col - 5));
}

static inline u8 seq_vel_to_pad(u8 level)
{
    return (u8)(10 * (1 + level / 4) + (5 + level % 4));
}

// right-column round buttons beside the lower grid half (rows 1..4,
// bottom-up) solo tracks 0..3; returns SEQ_NO_PAD elsewhere
static inline u8 seq_pad_to_solo(u8 index)
{
    u8 row = index / 10;
    u8 col = index % 10;

    if (col == 9 && row >= 1 && row <= SEQ_TRACK_COUNT)
    {
        return (u8)(row - 1);
    }

    return SEQ_NO_PAD;
}

static inline u8 seq_solo_to_pad(u8 track)
{
    return (u8)(10 * (track + 1) + 9);
}

// right-column round buttons beside the step rows (rows 5..8) trigger the
// beat repeat / fill; returns the rate slot 0..3 (bottom-up: 1/32, 1/16,
// 1/8, 1/4) or SEQ_NO_PAD elsewhere
static inline u8 seq_pad_to_fill(u8 index)
{
    u8 row = index / 10;
    u8 col = index % 10;

    if (col == 9 && row >= 5 && row <= 8)
    {
        return (u8)(row - 5);
    }

    return SEQ_NO_PAD;
}

#endif
