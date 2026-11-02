#include "app.h"
#include "config.h"
#include "layout.h"
#include "engine.h"
#include "clock.h"
#include "store.h"
#include "ui.h"

typedef struct
{
    u8 r, g, b;
} seq_color_t;

// one colour per track: amber, violet, green, blue (red is reserved for
// record/dynamic-velocity signalling)
static const seq_color_t TRACK_COLORS[SEQ_TRACK_COUNT] =
{
    {63, 20,  0},
    {32,  0, 63},
    { 0, 63,  0},
    { 0, 16, 63},
};

// ____________________________________________________________________________
//
// Modes. The sequencer engine and all side buttons stay live in every mode;
// only the 8x8 grid changes function.
// ____________________________________________________________________________

enum
{
    MODE_SESSION = 0,               // step sequencer (the main view)
    MODE_NOTE,                      // 8x8 playing surface with aftertouch
    MODE_DEVICE,                    // 8 gliding CC column faders
    MODE_USER                       // 64 assignable pads with LED feedback
};

static u8 s_mode;

static u8 s_selected_track;

// drum pad state (session mode): per held pad, the note/channel captured
// at press time (so releases match even if bank/track change)
static u8 s_pad_note[DRUM_PAD_COUNT];
static u8 s_pad_channel[DRUM_PAD_COUNT];

#define PAD_NOTE_NONE 0xFF

// entry velocity chosen on the velocity selector; 0 = dynamic mode,
// where each punched step takes the velocity of the actual pad hit
static u8 s_current_velocity;

// hit velocity of each held step, captured at press time
static u8 s_held_velocity[SEQ_STEP_COUNT];

// steps held down for hold-to-edit: bit set = held; a step whose
// "modified" bit gets set is not toggled on release. Hold-editing only
// engages after SEQ_HOLD_MS so quick taps stay plain toggles. Each
// step's own press time (against the free-running s_now_ms) decides
// whether ITS release still counts as a tap.
static u32 s_held_steps;
static u32 s_held_modified;
static u16 s_hold_ms;
static u16 s_now_ms;
static u16 s_step_press_ms[SEQ_STEP_COUNT];

static u8 hold_edit_active(void)
{
    return s_held_steps && s_hold_ms >= SEQ_HOLD_MS;
}

// capture (record) mode: incoming/played notes are quantized into the
// selected track while the transport runs
static u8 s_capture_armed;
static u8 s_capture_quantize;       // grid in ticks: 12, 6 or 3

// held track-select buttons: while any is down long enough, the session
// grid becomes the track's settings overlay - the same SEQ_HOLD_MS delay
// keeps quick track-select taps from flashing it
static u8 s_track_held;
static u16 s_track_hold_ms;

static u8 track_overlay_active(void)
{
    return s_track_held && s_track_hold_ms >= SEQ_HOLD_MS;
}

// Delete is destructive: it fires only after a long hold
static u8 s_clear_held;
static u16 s_clear_ms;
static u8 s_clear_done;

// Shift+Setup (total reset) gets the same protection
static u8 s_reset_held;
static u16 s_reset_ms;
static u8 s_reset_done;

// Shift held: drum pads select silently
static u8 s_shift_held;

// pattern pad held in the track overlay: tap selects for editing, a long
// hold toggles chain membership
static u8 s_patpad;                 // 0xFF = none
static u16 s_patpad_ms;
static u8 s_patpad_done;
static u8 s_drawn_activepat;

// mode buttons: tap = switch and stay, hold = momentary (revert on release)
static u8 s_mode_btn;               // held mode button index, 0xFF = none
static u16 s_mode_hold_ms;
static u8 s_mode_return;

// fill rates for the right-column buttons, bottom-up: 1/32, 1/16, 1/8, 1/4
static const u8 FILL_RATES[4] =
{
    SEQ_QUANT_32ND, SEQ_QUANT_16TH, SEQ_QUANT_8TH, SEQ_PPQN
};

// note mode state
static s8 s_note_octave;
static u8 s_note_aftertouch;        // poly aftertouch on/off
static u8 s_note_held[64];          // note per held grid pad, 0xFF = none
static u8 s_note_held_ch[64];

static u8 s_note_root;              // 0..11, semitones above C
static u8 s_note_scale;             // index into SCALE_IV
static u8 s_note_scalemode;         // 1 = scale layout, 0 = chromatic
static s8 s_note_transpose;         // semitones
static u8 s_note_channel;           // note mode's own MIDI channel

// the settings page is a latched view, opened/closed with Shift+Note
// (a plain Note tap also closes it)
static u8 s_note_setup_on;

static u8 note_setup_active(void)
{
    return s_note_setup_on;
}

// metronome on the User button: 0 = off, 1 = beat flash, 2 = "1" flash.
// The flash lasts a 16th, so its length follows the tempo.
static u8 metro_state(void)
{
    if (!g_seq.playing)
    {
        return 0;
    }

    u8 tick = seq_engine_bar_tick();
    u8 beat = SEQ_BAR_TICKS / 4;

    if ((tick % beat) >= SEQ_TICKS_PER_STEP)
    {
        return 0;
    }
    return (tick < beat) ? 2 : 1;
}

// the Launchpad X scale set
static const u8 SCALE_LEN[SEQ_SCALE_COUNT] =
{
    7, 7, 7, 7, 7, 7, 7, 8, 6, 5, 7, 7, 7, 7, 6, 5
};

static const u8 SCALE_IV[SEQ_SCALE_COUNT][8] =
{
    {0, 2, 3, 5, 7, 8, 10},         // natural minor
    {0, 2, 4, 5, 7, 9, 11},         // major
    {0, 2, 3, 5, 7, 9, 10},         // dorian
    {0, 1, 3, 5, 7, 8, 10},         // phrygian
    {0, 2, 4, 5, 7, 9, 10},         // mixolydian
    {0, 2, 3, 5, 7, 9, 11},         // melodic minor (ascending)
    {0, 2, 3, 5, 7, 8, 11},         // harmonic minor
    {0, 2, 3, 4, 5, 7, 9, 10},      // bebop dorian
    {0, 3, 5, 6, 7, 10},            // blues
    {0, 3, 5, 7, 10},               // minor pentatonic
    {0, 2, 3, 6, 7, 8, 11},         // hungarian minor
    {0, 2, 3, 6, 7, 9, 10},         // ukrainian dorian
    {0, 1, 4, 6, 7, 9, 11},         // marva
    {0, 1, 3, 6, 7, 8, 11},         // todi
    {0, 2, 4, 6, 8, 10},            // whole tone
    {0, 2, 3, 7, 8},                // hirajoshi
};

// is a semitone (relative to the root) part of the current scale?
static u8 in_scale(u8 rel)
{
    for (u8 i = 0; i < SCALE_LEN[s_note_scale]; ++i)
    {
        if (SCALE_IV[s_note_scale][i] == rel)
        {
            return 1;
        }
    }
    return 0;
}

// the piano-style root selector: semitone for a pad of the two viewer
// rows, 0xFF for the blank positions
static const u8 WHITE_KEYS[8] = {0, 2, 4, 5, 7, 9, 11, 0xFF};
static const u8 BLACK_KEYS[8] = {0xFF, 1, 3, 0xFF, 6, 8, 10, 0xFF};

// device mode: 8 gliding faders
static u8 s_fader_cur[8];
static u8 s_fader_target[8];
static u8 s_fader_speed[8];         // CC units per update tick
static u8 s_fader_ms;

// per-column fader colours: red, amber, yellow, green, cyan, blue,
// purple, pink
static const seq_color_t FADER_COLORS[8] =
{
    {63,  0,  0}, {63, 20,  0}, {50, 50,  0}, { 0, 63,  0},
    { 0, 40, 40}, { 0, 10, 63}, {30,  0, 63}, {63,  0, 30},
};

// user mode: LED palette index per grid pad, addressed by pad index,
// plus which pads are physically held (their ch16 note-offs must not be
// lost across mode switches)
static u8 s_user_led[100];
static u8 s_user_held[100];

// fill-rate buttons currently held (bitmask, one bit per rate)
static u8 s_fill_btns;

// approximation of the standard 128-colour Launchpad palette, built at
// init (0 = off, 1..3 = greys/white, then hue groups)
static u8 s_palette[128][3];

// ____________________________________________________________________________
//
// Rendering. The LED matrix is scanned by an unbuffered HAL interrupt, so
// both redundant hal_plot_led writes and periodic CPU bursts in the 1ms
// timer make columns shimmer. Three rules keep it calm:
//   1. a shadow framebuffer suppresses writes that change nothing
//   2. repaints happen only when state changed (event-driven, no fixed rate)
//   3. full repaints are sliced to a few pads per 1ms tick
// The moving playhead and gliding faders bypass the full repaint with
// targeted single-pad updates.
// ____________________________________________________________________________

static u8 s_shadow[100][3];
static u8 s_dirty;                  // full repaint requested
static u8 s_paint_index;            // 100 = idle, else repaint walk position
static u8 s_drawn_playhead;         // step the playhead is painted on
static u8 s_drawn_playing;
static u8 s_drawn_external;
static u8 s_drawn_fill;
static u8 s_drawn_armed;
static u8 s_drawn_metro;

// capture auto-disarm: pattern-end count of the selected track at arming
// (rebased on track switches and play-state edges)
static u8 s_cap_track;
static u8 s_cap_playing;
static u8 s_cap_wraps_base;

#define PADS_PER_TICK 10

static void plot(u8 index, u8 r, u8 g, u8 b)
{
    u8 *s = s_shadow[index];

    if (s[0] == r && s[1] == g && s[2] == b)
    {
        return;
    }

    s[0] = r;
    s[1] = g;
    s[2] = b;
    hal_plot_led(TYPEPAD, index, r, g, b);
}

// ____________________________________________________________________________
//
// Palette (h in degrees 0..359, s/v in 0..63)
// ____________________________________________________________________________

static void hsv_to_rgb(u16 h, u8 s, u8 v, u8 *r, u8 *g, u8 *b)
{
    u8 region = (u8)(h / 60);
    u16 rem = h % 60;

    u8 p = (u8)((u16)v * (63 - s) / 63);
    u8 q = (u8)((u16)v * (63 - (u16)s * rem / 60) / 63);
    u8 t = (u8)((u16)v * (63 - (u16)s * (60 - rem) / 60) / 63);

    switch (region)
    {
        case 0:  *r = v; *g = t; *b = p; break;
        case 1:  *r = q; *g = v; *b = p; break;
        case 2:  *r = p; *g = v; *b = t; break;
        case 3:  *r = p; *g = q; *b = v; break;
        case 4:  *r = t; *g = p; *b = v; break;
        default: *r = v; *g = p; *b = q; break;
    }
}

static void build_palette(void)
{
    s_palette[0][0] = 0;  s_palette[0][1] = 0;  s_palette[0][2] = 0;
    s_palette[1][0] = 15; s_palette[1][1] = 15; s_palette[1][2] = 15;
    s_palette[2][0] = 35; s_palette[2][1] = 35; s_palette[2][2] = 35;
    s_palette[3][0] = 63; s_palette[3][1] = 63; s_palette[3][2] = 63;

    // 4..59: 14 hue groups of 4 shades (pale, bright, mid, dim)
    for (u8 i = 4; i < 60; ++i)
    {
        u8 group = (u8)((i - 4) / 4);
        u8 shade = (u8)((i - 4) % 4);
        u16 hue = (u16)(group * 360 / 14);

        switch (shade)
        {
            case 0:  hsv_to_rgb(hue, 25, 63, &s_palette[i][0],
                                &s_palette[i][1], &s_palette[i][2]); break;
            case 1:  hsv_to_rgb(hue, 63, 63, &s_palette[i][0],
                                &s_palette[i][1], &s_palette[i][2]); break;
            case 2:  hsv_to_rgb(hue, 63, 25, &s_palette[i][0],
                                &s_palette[i][1], &s_palette[i][2]); break;
            default: hsv_to_rgb(hue, 63, 10, &s_palette[i][0],
                                &s_palette[i][1], &s_palette[i][2]); break;
        }
    }

    // 60..127: a second full-brightness sweep of the hue wheel
    for (u8 i = 60; i < 128; ++i)
    {
        hsv_to_rgb((u16)((i - 60) * 360 / 68), 63, 63,
                   &s_palette[i][0], &s_palette[i][1], &s_palette[i][2]);
    }
}

// ____________________________________________________________________________
//
// Session mode grid colours
// ____________________________________________________________________________

static s16 drum_pad_note(u8 pad)
{
    s16 note = DRUM_BASE_NOTE
             + DRUM_BANK_SIZE * g_seq.track[s_selected_track].drum_bank
             + pad;

    return (note >= 0 && note <= 127) ? note : -1;
}

static u8 level_velocity(u8 level)
{
    return (u8)((level + 1) * 8 - 1);       // 7, 15, ... 127
}

static u8 velocity_level(u8 velocity)
{
    return velocity >> 3;                   // inverse, 0..15
}

static void step_color(u8 step, u8 *r, u8 *g, u8 *b)
{
    const seq_color_t *c = &TRACK_COLORS[s_selected_track];
    const seq_track_t *track = &g_seq.track[s_selected_track];

    // the playhead shows only while viewing the pattern that is sounding
    if (g_seq.playing && step == seq_engine_track_pos(s_selected_track) &&
        g_seq.track[s_selected_track].edit_pat ==
        g_seq.track[s_selected_track].active_pat)
    {
        *r = MAXLED; *g = MAXLED; *b = MAXLED;      // playhead
        return;
    }

    // steps beyond the pattern's loop length rest dark (but stay editable)
    if (step >= seq_engine_length(s_selected_track))
    {
        u8 velocity = seq_engine_note_velocity(s_selected_track, step,
                                               track->cursor_note);
        u8 v = velocity ? 4 : 1;

        *r = v; *g = v; *b = v;
        return;
    }

    u8 velocity = seq_engine_note_velocity(s_selected_track, step,
                                           track->cursor_note);
    if (velocity)
    {
        // active step of the shown note lane, brightness follows velocity
        u16 level = 20 + ((u16)velocity * 43) / 127;
        *r = (u8)(c->r * level / MAXLED);
        *g = (u8)(c->g * level / MAXLED);
        *b = (u8)(c->b * level / MAXLED);

        // steps whose lane note is nudged get a white lift
        if (seq_engine_step_nudge(s_selected_track, step,
                                  track->cursor_note))
        {
            *r = (*r > MAXLED - 12) ? MAXLED : (u8)(*r + 12);
            *g = (*g > MAXLED - 12) ? MAXLED : (u8)(*g + 12);
            *b = (*b > MAXLED - 12) ? MAXLED : (u8)(*b + 12);
        }
    }
    else
    {
        // uniform background - placed steps must stand out unambiguously
        *r = c->r >> 3; *g = c->g >> 3; *b = c->b >> 3;
    }
}

static void drum_color(u8 pad, u8 *r, u8 *g, u8 *b)
{
    const seq_color_t *c = &TRACK_COLORS[s_selected_track];
    const seq_track_t *track = &g_seq.track[s_selected_track];
    s16 note = drum_pad_note(pad);

    *r = 0; *g = 0; *b = 0;

    // while steps are held, this 4x4 is a probability scale (1/16..16/16)
    if (hold_edit_active())
    {
        u8 prob = SEQ_PROB_MAX;

        for (u8 step = 0; step < SEQ_STEP_COUNT; ++step)
        {
            if (s_held_steps & (1UL << step))
            {
                prob = seq_engine_step_prob(s_selected_track, step,
                                            track->cursor_note);
                break;
            }
        }

        if (pad + 1 == prob)
        {
            *r = MAXLED; *g = MAXLED; *b = MAXLED;
        }
        else if (pad + 1 < prob)
        {
            *r = 25; *g = 12;                       // filled: amber
        }
        else
        {
            *r = 3; *g = 1;
        }
        return;
    }

    if (note < 0)
    {
        return;                                     // out of MIDI range
    }
    if (s_pad_note[pad] != PAD_NOTE_NONE)
    {
        *r = MAXLED; *g = MAXLED; *b = MAXLED;      // held
    }
    else if ((u8)note == track->cursor_note)
    {
        *r = c->r; *g = c->g; *b = c->b;            // the shown note lane
    }
    else if (seq_engine_note_used(s_selected_track, (u8)note))
    {
        // note has steps somewhere in this track
        *r = c->r >> 2; *g = c->g >> 2; *b = c->b >> 2;
    }
    else if (note % 12 == 0)
    {
        *r = 5; *g = 5; *b = 14;                    // mark the Cs
    }
    else
    {
        *g = 6; *b = 6;         // cyan base sets the drum area apart
    }
}

static void vel_color(u8 level, u8 *r, u8 *g, u8 *b)
{
    const seq_color_t *c = &TRACK_COLORS[s_selected_track];
    const seq_track_t *track = &g_seq.track[s_selected_track];

    // while steps are held, show their current velocity in white
    for (u8 step = 0; hold_edit_active() && step < SEQ_STEP_COUNT; ++step)
    {
        if (!(s_held_steps & (1UL << step)))
        {
            continue;
        }

        u8 velocity = seq_engine_note_velocity(s_selected_track, step,
                                               track->cursor_note);
        if (velocity && velocity_level(velocity) == level)
        {
            *r = MAXLED; *g = MAXLED; *b = MAXLED;
            return;
        }
    }

    if (s_current_velocity && level == velocity_level(s_current_velocity))
    {
        *r = c->r; *g = c->g; *b = c->b;            // selected entry level
    }
    else
    {
        // gradient: brighter pad = louder level. White in fixed mode; red
        // when dynamic (hit-strength) mode is on - red is the "recording
        // dynamics" colour, distinct from every track colour
        u8 v = 2 + level;

        if (s_current_velocity == 0)
        {
            *r = (u8)(v + 2);
        }
        else
        {
            *r = v; *g = v; *b = v;
        }
    }
}

// while a track button is held, the grid becomes the track's settings:
//   rows 8..5  pattern length (tap = new last step)
//   row 4      cols 1-4: pattern chain toggles / cols 5-8: gate length
//   rows 3..2  MIDI channel 1..16
//   row 1      step rate
static void overlay_color(u8 index, u8 *r, u8 *g, u8 *b)
{
    const seq_color_t *c = &TRACK_COLORS[s_selected_track];
    const seq_track_t *track = &g_seq.track[s_selected_track];
    u8 length = seq_engine_length(s_selected_track);

    *r = 0; *g = 0; *b = 0;

    u8 step = seq_pad_to_step(index);

    if (step != SEQ_NO_STEP)
    {
        // playhead only while the viewed pattern is the sounding one -
        // otherwise it would freeze (no targeted repaint runs then)
        if (g_seq.playing &&
            track->edit_pat == track->active_pat &&
            step == seq_engine_track_pos(s_selected_track))
        {
            *r = MAXLED; *g = MAXLED; *b = MAXLED;
        }
        else if (step == length - 1)
        {
            *r = MAXLED; *g = MAXLED; *b = MAXLED;  // the last step
        }
        else if (step < length)
        {
            *r = c->r >> 1; *g = c->g >> 1; *b = c->b >> 1;
        }
        else
        {
            *r = 1; *g = 1; *b = 1;
        }
        return;
    }

    u8 grid = seq_pad_to_grid(index);

    if (grid < SEQ_RATE_COUNT)                      // row 1: rates
    {
        if (SEQ_RATES[grid] == track->rate)
        {
            *r = MAXLED; *g = MAXLED; *b = MAXLED;
        }
        else
        {
            *r = c->r >> 3; *g = c->g >> 3; *b = c->b >> 3;
        }
    }
    else if (grid < 24)                             // rows 2-3: channel
    {
        u8 ch = (u8)(grid - 8);

        if (ch == track->channel)
        {
            *r = MAXLED; *g = MAXLED; *b = MAXLED;
        }
        else
        {
            *r = c->r >> 3; *g = c->g >> 3; *b = c->b >> 3;
        }
    }
    else if (grid < 24 + SEQ_PATTERN_COUNT)         // row 4: patterns
    {
        u8 p = (u8)(grid - 24);

        if (p == track->active_pat)
        {
            *r = MAXLED; *g = MAXLED; *b = MAXLED;  // playing now
        }
        else
        {
            // green = chained, red = not; blue tint = viewed/edited
            if (track->chain_mask & (1 << p))
            {
                *g = 32;
            }
            else
            {
                *r = 14;
            }
            if (p == track->edit_pat)
            {
                *b = 30;
            }
        }
    }
    else if (grid >= 28 && grid < 32)               // row 4: gate length
    {
        u8 gq = (u8)(grid - 28 + 1);

        if (gq == track->gate)
        {
            *r = MAXLED; *g = MAXLED; *b = MAXLED;
        }
        else
        {
            u8 v = (u8)(3 + gq * 3);

            *r = v; *g = v >> 1; *b = 0;
        }
    }
}

static void session_grid_color(u8 index, u8 *r, u8 *g, u8 *b)
{
    if (track_overlay_active())
    {
        overlay_color(index, r, g, b);
        return;
    }

    u8 step = seq_pad_to_step(index);
    if (step != SEQ_NO_STEP)
    {
        step_color(step, r, g, b);
        return;
    }

    u8 drum = seq_pad_to_drum(index);
    if (drum != SEQ_NO_PAD)
    {
        drum_color(drum, r, g, b);
        return;
    }

    u8 level = seq_pad_to_vel(index);
    if (level != SEQ_NO_PAD)
    {
        vel_color(level, r, g, b);
    }
}

// ____________________________________________________________________________
//
// Note mode
// ____________________________________________________________________________

static s16 note_grid_note(u8 grid)
{
    s16 note;

    if (s_note_scalemode)
    {
        // pads are scale degrees, rows a 4th of degrees apart
        u8 len = SCALE_LEN[s_note_scale];
        u8 d = (u8)((grid / 8) * NOTE_SCALE_INTERVAL + (grid % 8));

        note = NOTE_MODE_BASE + s_note_root + 12 * s_note_octave
             + s_note_transpose + 12 * (d / len)
             + SCALE_IV[s_note_scale][d % len];
    }
    else
    {
        note = NOTE_MODE_BASE + 12 * s_note_octave + s_note_transpose
             + (grid / 8) * NOTE_MODE_INTERVAL + (grid % 8);
    }

    return (note >= 0 && note <= 127) ? note : -1;
}

// the playing surface: held keys white, roots purple; chromatic layout
// additionally marks in-scale notes blue
static void note_grid_color(u8 grid, u8 *r, u8 *g, u8 *b)
{
    s16 note = note_grid_note(grid);

    *r = 0; *g = 0; *b = 0;

    if (note < 0)
    {
        return;
    }
    if (s_note_held[grid] != PAD_NOTE_NONE)
    {
        *r = MAXLED; *g = MAXLED; *b = MAXLED;
        return;
    }

    u8 rel = (u8)(((note % 12) - s_note_root + 12) % 12);

    if (rel == 0)
    {
        *r = 22; *b = 30;                           // root
    }
    else if (s_note_scalemode)
    {
        *r = 3; *g = 3; *b = 6;                     // all pads in scale
    }
    else if (in_scale(rel))
    {
        *b = 16; *g = 6;                            // in scale
    }
    else
    {
        *r = 2; *g = 2; *b = 2;
    }
}

// ____________________________________________________________________________
//
// Note settings page (Shift+Note toggles it; a plain Note tap closes it):
//   row 7 (top)  transpose - / +, chromatic/scale toggle at the right
//   rows 6-5     piano-style root select + scale viewer
//   rows 4-3     the 16 scales
//   rows 1-0     MIDI channel 1..16
// ____________________________________________________________________________

static void note_setup_color(u8 grid, u8 *r, u8 *g, u8 *b)
{
    u8 row = grid / 8;
    u8 col = grid % 8;

    *r = 0; *g = 0; *b = 0;

    if (row == 7)
    {
        if (col == 0)
        {
            u8 v = (s_note_transpose < 0) ? 25 : 8;

            *r = v; *g = v; *b = v;                 // transpose down
        }
        else if (col == 1)
        {
            u8 v = (s_note_transpose > 0) ? 25 : 8;

            *r = v; *g = v; *b = v;                 // transpose up
        }
        else if (col == 7)
        {
            // chromatic/scale toggle, orange - bright in scale mode
            if (s_note_scalemode)
            {
                *r = 50; *g = 15;
            }
            else
            {
                *r = 14; *g = 4;
            }
        }
        return;
    }

    if (row == 6 || row == 5)
    {
        u8 semi = (row == 6) ? BLACK_KEYS[col] : WHITE_KEYS[col];

        if (semi == 0xFF)
        {
            return;
        }
        if (semi == s_note_root)
        {
            *r = 30; *b = 40;                       // root: purple
        }
        else if (in_scale((u8)((semi - s_note_root + 12) % 12)))
        {
            *b = 30; *g = 12;                       // in scale: blue
        }
        else
        {
            *r = 5; *g = 5; *b = 6;
        }
        return;
    }

    if (row == 4 || row == 3)
    {
        u8 scale = (u8)((4 - row) * 8 + col);

        if (scale == s_note_scale)
        {
            *r = 50; *g = 50; *b = 20;              // selected scale
        }
        else
        {
            *r = 4; *g = 6; *b = 9;
        }
        return;
    }

    if (row == 1 || row == 0)
    {
        u8 ch = (u8)((1 - row) * 8 + col);

        if (ch == s_note_channel)
        {
            *r = 50; *g = 50; *b = 15;              // selected channel
        }
        else
        {
            *r = 8; *g = 7; *b = 2;
        }
    }
}

static void note_setup_event(u8 grid)
{
    u8 row = grid / 8;
    u8 col = grid % 8;

    if (row == 7)
    {
        if (col == 0 && s_note_transpose > -NOTE_TRANSPOSE_MAX)
        {
            --s_note_transpose;
        }
        else if (col == 1 && s_note_transpose < NOTE_TRANSPOSE_MAX)
        {
            ++s_note_transpose;
        }
        else if (col == 7)
        {
            s_note_scalemode = !s_note_scalemode;
        }
        return;
    }

    if (row == 6 || row == 5)
    {
        u8 semi = (row == 6) ? BLACK_KEYS[col] : WHITE_KEYS[col];

        if (semi != 0xFF)
        {
            s_note_root = semi;
        }
        return;
    }

    if (row == 4 || row == 3)
    {
        s_note_scale = (u8)((4 - row) * 8 + col);
        return;
    }

    if (row == 1 || row == 0)
    {
        s_note_channel = (u8)((1 - row) * 8 + col);
    }
}

// ____________________________________________________________________________
//
// Device mode (column faders)
// ____________________________________________________________________________

static void fader_grid_color(u8 grid, u8 *r, u8 *g, u8 *b)
{
    u8 col = grid % 8;
    u8 row = grid / 8;                      // 0 = bottom
    const seq_color_t *c = &FADER_COLORS[col];

    // each row spans 16 CC units; light fully below the level, scale the
    // boundary row for a smooth fill
    u16 above = (u16)row * 16;
    u8 cur = s_fader_cur[col];
    u8 amount;

    if (cur >= above + 16)
    {
        amount = 16;
    }
    else if (cur > above)
    {
        amount = (u8)(cur - above);
    }
    else
    {
        amount = 0;
    }

    if (amount)
    {
        u8 lift = (u8)(15 + amount * 3);    // 18..63
        *r = (u8)((u16)c->r * lift / MAXLED);
        *g = (u8)((u16)c->g * lift / MAXLED);
        *b = (u8)((u16)c->b * lift / MAXLED);
    }
    else
    {
        // stay above the PWM floor - brightness 1-2 LEDs shimmer
        *r = c->r >> 3; *g = c->g >> 3; *b = c->b >> 3;
    }
}

static void repaint_fader_column(u8 col);

static void update_faders(void)
{
    for (u8 col = 0; col < 8; ++col)
    {
        u8 cur = s_fader_cur[col];
        u8 target = s_fader_target[col];

        if (cur == target)
        {
            continue;
        }

        u8 speed = s_fader_speed[col] ? s_fader_speed[col] : 1;

        if (cur < target)
        {
            cur = (target - cur > speed) ? (u8)(cur + speed) : target;
        }
        else
        {
            cur = (cur - target > speed) ? (u8)(cur - speed) : target;
        }
        s_fader_cur[col] = cur;

#if SEQ_SEND_USB
        hal_send_midi(SEQ_USB_PORT, CC | SEQ_CTRL_CHANNEL,
                      SEQ_FADER_CC_BASE + col, cur);
#endif
#if SEQ_SEND_DIN
        hal_send_midi(DINMIDI, CC | SEQ_CTRL_CHANNEL,
                      SEQ_FADER_CC_BASE + col, cur);
#endif

        if (s_mode == MODE_DEVICE)
        {
            repaint_fader_column(col);
        }
    }
}

// ____________________________________________________________________________
//
// Shared rendering
// ____________________________________________________________________________

static void target_color(u8 index, u8 *r, u8 *g, u8 *b)
{
    *r = 0; *g = 0; *b = 0;

    u8 grid = seq_pad_to_grid(index);

    if (grid != SEQ_NO_PAD)
    {
        switch (s_mode)
        {
            case MODE_SESSION:
                session_grid_color(index, r, g, b);
                break;

            case MODE_NOTE:
                if (note_setup_active())
                {
                    note_setup_color(grid, r, g, b);
                }
                else
                {
                    note_grid_color(grid, r, g, b);
                }
                break;

            case MODE_DEVICE:
                fader_grid_color(grid, r, g, b);
                break;

            default:
            {
                const u8 *p = s_palette[s_user_led[index] & 0x7F];
                *r = p[0]; *g = p[1]; *b = p[2];
                break;
            }
        }
        return;
    }

    if (index >= BTN_TRACK_BASE && index < BTN_TRACK_BASE + SEQ_TRACK_COUNT)
    {
        u8 t = index - BTN_TRACK_BASE;
        const seq_color_t *c = &TRACK_COLORS[t];

        if (t == s_selected_track)
        {
            *r = c->r; *g = c->g; *b = c->b;
        }
        else
        {
            *r = c->r >> 3; *g = c->g >> 3; *b = c->b >> 3;
        }
        return;
    }

    if (index >= BTN_MUTE_BASE && index < BTN_MUTE_BASE + SEQ_TRACK_COUNT)
    {
        u8 t = index - BTN_MUTE_BASE;
        const seq_color_t *c = &TRACK_COLORS[t];

        if (g_seq.track[t].muted)
        {
            *r = 1; *g = 1; *b = 1;                 // muted = dark
        }
        else
        {
            *r = c->r >> 1; *g = c->g >> 1; *b = c->b >> 1;
        }
        return;
    }

    u8 solo = seq_pad_to_solo(index);
    if (solo != SEQ_NO_PAD)
    {
        if (g_seq.track[solo].solo)
        {
            *r = MAXLED; *g = MAXLED;               // bright yellow
        }
        else
        {
            *r = 3; *g = 3;
        }
        return;
    }

    u8 fill = seq_pad_to_fill(index);
    if (fill != SEQ_NO_PAD)
    {
        if (seq_engine_fill_rate() == FILL_RATES[fill])
        {
            *r = 40; *b = MAXLED;                   // active: bright purple
        }
        else
        {
            *r = 6; *b = 10;
        }
        return;
    }

    switch (index)
    {
        case BTN_PLAY:
            if (seq_clock_is_armed())
            {
                *r = MAXLED; *g = MAXLED; *b = MAXLED;  // start pending
            }
            else if (!g_seq.playing)
            {
                if (seq_clock_is_external())
                {
                    *r = 10; *g = 5;    // stopped, external clock present
                }
                else
                {
                    *g = 8;
                }
            }
            else if (seq_clock_is_external())
            {
                *r = 40; *g = 20;                   // slaved to MIDI clock
            }
            else
            {
                *g = MAXLED;
            }
            break;

        case BTN_TEMPO_DOWN:
        case BTN_TEMPO_UP:
            *r = 6; *g = 6; *b = 6;
            break;

        case BTN_QUANTIZE:
            if (s_capture_quantize == SEQ_QUANT_8TH)
            {
                *b = 35; *g = 10;                   // 8th: blue
            }
            else if (s_capture_quantize == SEQ_QUANT_16TH)
            {
                *r = 8; *g = 8; *b = 8;             // 16th: white
            }
            else
            {
                *r = 35; *g = 15;                   // 32nd: amber
            }
            break;

        case BTN_SESSION:
            if (s_mode == MODE_SESSION) { *g = 50; *b = 15; }
            else                        { *g = 5;  *b = 1;  }
            break;

        case BTN_NOTE:
            if (s_mode == MODE_NOTE)
            {
                // bright when aftertouch is on, muted blue when off
                if (s_note_aftertouch) { *b = MAXLED; *g = 20; }
                else                   { *b = 30; *g = 10; }
            }
            else
            {
                *b = 8; *g = 2;
            }
            break;

        case BTN_DEVICE:
            if (s_mode == MODE_DEVICE) { *r = 50; *g = 25; }
            else                       { *r = 6;  *g = 3;  }
            break;

        case BTN_USER:
        {
            // doubles as the metronome while playing: white on the "1",
            // pink on beats 2-4
            u8 metro = metro_state();

            if (metro == 2)
            {
                *r = MAXLED; *g = MAXLED; *b = MAXLED;
            }
            else if (metro == 1)
            {
                *r = 45; *g = 12; *b = 30;
            }
            else if (s_mode == MODE_USER)
            {
                *r = 50; *b = 30;
            }
            else
            {
                *r = 6; *b = 4;
            }
            break;
        }

        case BTN_BANK_UP:
        {
            s8 v = (s_mode == MODE_NOTE)
                 ? s_note_octave
                 : g_seq.track[s_selected_track].drum_bank;
            *r = (v > 0) ? 20 : 6;
            *g = (v > 0) ? 20 : 6;
            *b = 6;
            break;
        }

        case BTN_BANK_DOWN:
        {
            s8 v = (s_mode == MODE_NOTE)
                 ? s_note_octave
                 : g_seq.track[s_selected_track].drum_bank;
            *r = (v < 0) ? 20 : 6;
            *g = (v < 0) ? 20 : 6;
            *b = 6;
            break;
        }

        case BTN_RECORD:
            *r = s_capture_armed ? MAXLED : 6;
            break;

        case BTN_CLEAR:
            if (s_clear_done)
            {
                *g = MAXLED;                    // cleared
            }
            else if (s_clear_held)
            {
                *r = 40;                        // hold to confirm...
            }
            else
            {
                *r = 4; *g = 4; *b = 4;
            }
            break;

        case BTN_SHIFT:
            if (s_shift_held)
            {
                *r = 30; *g = 30; *b = 30;
            }
            else
            {
                *r = 5; *g = 5; *b = 5;
            }
            break;

        case BTN_DOUBLE:
            if (seq_engine_clipboard_full())
            {
                *g = 25; *b = 25;       // holds a pattern - paste ready
            }
            else
            {
                *r = 4; *g = 4; *b = 4;
            }
            break;

        case BTN_SWING_DOWN:
            *g = 4; *b = 4;
            break;

        case BTN_SWING_UP:
        {
            // brightness shows the current swing amount
            u8 v = (u8)(4 + seq_engine_swing() * 9);

            *g = v; *b = v;
            break;
        }

        default:
            break;
    }
}

static void paint_pad(u8 index)
{
    u8 r, g, b;

    target_color(index, &r, &g, &b);
    plot(index, r, g, b);
}

static void repaint_fader_column(u8 col)
{
    for (u8 row = 0; row < 8; ++row)
    {
        paint_pad(seq_grid_to_pad((u8)(row * 8 + col)));
    }
}

void seq_ui_init(void)
{
    s_mode = MODE_SESSION;
    s_selected_track = 0;
    s_current_velocity = SEQ_DEFAULT_VELOCITY;
    s_held_steps = 0;
    s_held_modified = 0;
    s_hold_ms = 0;
    s_capture_armed = 0;
    s_capture_quantize = SEQ_QUANT_16TH;
    s_track_held = 0;
    s_track_hold_ms = 0;
    s_clear_held = 0;
    s_clear_ms = 0;
    s_clear_done = 0;
    s_reset_held = 0;
    s_reset_ms = 0;
    s_reset_done = 0;
    s_shift_held = 0;
    s_patpad = 0xFF;
    s_patpad_ms = 0;
    s_patpad_done = 0;
    s_drawn_activepat = 0;
    s_mode_btn = 0xFF;
    s_mode_hold_ms = 0;
    s_mode_return = MODE_SESSION;

    s_note_octave = 0;
    s_note_aftertouch = 1;
    s_note_root = 0;
    s_note_scale = 0;               // natural minor
    s_note_scalemode = 0;           // chromatic layout
    s_note_transpose = 0;
    s_note_channel = 0;
    s_note_setup_on = 0;

    for (u8 pad = 0; pad < DRUM_PAD_COUNT; ++pad)
    {
        s_pad_note[pad] = PAD_NOTE_NONE;
    }
    for (u8 g = 0; g < 64; ++g)
    {
        s_note_held[g] = PAD_NOTE_NONE;
    }
    for (u8 col = 0; col < 8; ++col)
    {
        s_fader_cur[col] = 0;
        s_fader_target[col] = 0;
        s_fader_speed[col] = 0;
    }
    s_fader_ms = 0;
    for (u8 i = 0; i < 100; ++i)
    {
        s_user_led[i] = 0;
        s_user_held[i] = 0;
    }
    s_fill_btns = 0;
    s_now_ms = 0;

    build_palette();

    // invalidate the shadow so the first repaint writes every LED
    for (u8 i = 0; i < 100; ++i)
    {
        s_shadow[i][0] = 0xFF;
    }

    s_drawn_playhead = 0xFF;
    s_drawn_playing = 0;
    s_drawn_external = 0;
    s_drawn_fill = 0;
    s_drawn_armed = 0;
    s_drawn_metro = 0;
    s_cap_track = 0;
    s_cap_playing = 0;
    s_cap_wraps_base = 0;
    s_paint_index = 100;
    s_dirty = 1;
}

static void flush_holds(void);

// wipe the whole working state back to factory defaults. The flash save
// is untouched - a power-cycle restores the last saved state.
static void total_reset(void)
{
    seq_clock_halt();               // MIDI STOP out + disarm (needs playing)
    flush_holds();                  // note-offs for everything under fingers
    seq_engine_stop();              // note-offs for anything sequenced
    seq_engine_init();
    seq_clock_set_bpm(SEQ_DEFAULT_BPM);

    s_selected_track = 0;
    s_current_velocity = SEQ_DEFAULT_VELOCITY;
    s_capture_quantize = SEQ_QUANT_16TH;
    s_capture_armed = 0;
    s_note_octave = 0;
    s_note_aftertouch = 1;
    s_note_root = 0;
    s_note_scale = 0;
    s_note_scalemode = 0;
    s_note_transpose = 0;
    s_note_channel = 0;
    s_note_setup_on = 0;

    s_dirty = 1;
}

void seq_ui_setup_event(u8 pressed)
{
    if (pressed)
    {
        if (s_shift_held)
        {
            // arm the total reset - it fires after the long hold
            s_reset_held = 1;
            s_reset_ms = 0;
            s_reset_done = 0;
            hal_plot_led(TYPESETUP, 0, 20, 0, 0);   // arming...
        }
        else
        {
            seq_store_save();
            hal_plot_led(TYPESETUP, 0, 0, MAXLED, 0);
        }
    }
    else
    {
        s_reset_held = 0;
        hal_plot_led(TYPESETUP, 0, 0, 0, 0);
    }
}

void seq_ui_ms(void)
{
    ++s_now_ms;     // free-running, for per-step hold timing

    // transport and clock-source state can also change from MIDI input
    if (g_seq.playing != s_drawn_playing)
    {
        s_drawn_playing = g_seq.playing;
        s_dirty = 1;
    }

    u8 external = seq_clock_is_external();

    if (external != s_drawn_external)
    {
        s_drawn_external = external;
        s_dirty = 1;
    }

    // fills can end on their own at a bar line
    u8 fill = seq_engine_fill_rate();

    if (fill != s_drawn_fill)
    {
        s_drawn_fill = fill;
        s_dirty = 1;
    }

    // the metronome flash repaints only the User button
    u8 metro = metro_state();

    if (metro != s_drawn_metro)
    {
        s_drawn_metro = metro;
        paint_pad(BTN_USER);
    }

    // a pending bar-quantized start fires (or is cancelled) on its own
    u8 armed = seq_clock_is_armed();

    if (armed != s_drawn_armed)
    {
        s_drawn_armed = armed;
        s_dirty = 1;
    }

    // hold-edit engages once step pads have been held long enough
    if (s_held_steps)
    {
        if (s_hold_ms < 0xFFFF)
        {
            ++s_hold_ms;
        }
        if (s_hold_ms == SEQ_HOLD_MS)
        {
            s_dirty = 1;        // reveal the probability / velocity views
        }
    }
    else if (s_hold_ms)
    {
        s_hold_ms = 0;
        s_dirty = 1;
    }

    // time held mode buttons for the momentary switch
    if (s_mode_btn != 0xFF && s_mode_hold_ms < 0xFFFF)
    {
        ++s_mode_hold_ms;
    }

    // the total reset fires after its long hold
    if (s_reset_held && !s_reset_done)
    {
        if (++s_reset_ms >= SEQ_CLEAR_HOLD_MS)
        {
            total_reset();
            s_reset_done = 1;
            hal_plot_led(TYPESETUP, 0, MAXLED, 0, 0);   // wiped
        }
    }

    // Delete fires after its long hold
    if (s_clear_held && !s_clear_done)
    {
        if (++s_clear_ms >= SEQ_CLEAR_HOLD_MS)
        {
            seq_engine_clear_lane(s_selected_track,
                                  g_seq.track[s_selected_track].cursor_note);
            s_clear_done = 1;
            s_dirty = 1;
        }
    }

    // likewise for the track settings overlay
    if (s_track_held)
    {
        if (s_track_hold_ms < 0xFFFF)
        {
            ++s_track_hold_ms;
        }
        if (s_track_hold_ms == SEQ_HOLD_MS)
        {
            s_dirty = 1;        // reveal the overlay
        }
    }
    else if (s_track_hold_ms)
    {
        s_track_hold_ms = 0;
        s_dirty = 1;
    }

    // pattern pads in the overlay: a long hold toggles chain membership
    if (s_patpad != 0xFF)
    {
        if (!track_overlay_active() || s_mode != MODE_SESSION)
        {
            s_patpad = 0xFF;    // overlay closed under the finger
        }
        else if (!s_patpad_done && ++s_patpad_ms >= SEQ_MODE_HOLD_MS)
        {
            seq_engine_toggle_chain(s_selected_track, s_patpad);
            s_patpad_done = 1;
            s_dirty = 1;
        }
    }

    // the chain can move the playing pattern along; keep the overlay's
    // white marker (and the playhead gating) fresh
    u8 activepat = g_seq.track[s_selected_track].active_pat;

    if (activepat != s_drawn_activepat)
    {
        s_drawn_activepat = activepat;
        if (track_overlay_active())
        {
            s_dirty = 1;
        }
    }

    // capture disarms itself after one full pass of the selected track's
    // pattern, counted via the engine's pattern-end counter (robust for
    // length-1 patterns, restarts and chain switches)
    if (s_capture_armed)
    {
        if (s_cap_track != s_selected_track ||
            g_seq.playing != s_cap_playing)
        {
            // rebase on track switches and play-state edges
            s_cap_track = s_selected_track;
            s_cap_playing = g_seq.playing;
            s_cap_wraps_base = seq_engine_track_wraps(s_selected_track);
        }
        else if (g_seq.playing &&
                 (u8)(seq_engine_track_wraps(s_selected_track)
                      - s_cap_wraps_base) >= 2)
        {
            s_capture_armed = 0;
            s_dirty = 1;
        }
    }

    // follow the playhead by repainting just the two affected pads
    // (only while the viewed pattern is the sounding one)
    u8 head = (g_seq.playing && s_mode == MODE_SESSION &&
               g_seq.track[s_selected_track].edit_pat ==
               g_seq.track[s_selected_track].active_pat)
            ? seq_engine_track_pos(s_selected_track) : 0xFF;

    if (head != s_drawn_playhead)
    {
        u8 old = s_drawn_playhead;
        s_drawn_playhead = head;

        if (old != 0xFF)
        {
            paint_pad(seq_step_to_pad(old));
        }
        if (head != 0xFF)
        {
            paint_pad(seq_step_to_pad(head));
        }
    }

    // gliding faders
    if (++s_fader_ms >= SEQ_FADER_UPDATE_MS)
    {
        s_fader_ms = 0;
        update_faders();
    }

    // sliced full repaint
    if (s_paint_index < 100)
    {
        u8 end = s_paint_index + PADS_PER_TICK;

        for (; s_paint_index < end && s_paint_index < 100; ++s_paint_index)
        {
            paint_pad(s_paint_index);
        }
    }
    else if (s_dirty)
    {
        s_dirty = 0;
        s_paint_index = 0;
    }
}

u8 seq_ui_get_quantize(void)
{
    return s_capture_quantize;
}

void seq_ui_set_quantize(u8 ticks)
{
    if (ticks == SEQ_QUANT_8TH || ticks == SEQ_QUANT_32ND)
    {
        s_capture_quantize = ticks;
    }
    else
    {
        s_capture_quantize = SEQ_QUANT_16TH;
    }
    s_dirty = 1;
}

void seq_ui_get_note_setup(u8 *out)
{
    out[0] = s_note_root;
    out[1] = s_note_scale;
    out[2] = (u8)((s_note_scalemode ? 1 : 0) | (s_note_aftertouch ? 2 : 0));
    out[3] = (u8)s_note_transpose;
    out[4] = s_note_channel;
    out[5] = (u8)s_note_octave;
}

void seq_ui_set_note_setup(const u8 *in)
{
    s_note_root = in[0] % 12;
    s_note_scale = in[1] % SEQ_SCALE_COUNT;
    s_note_scalemode = in[2] & 1;
    s_note_aftertouch = (in[2] >> 1) & 1;

    s8 transpose = (s8)in[3];

    if (transpose >= -NOTE_TRANSPOSE_MAX && transpose <= NOTE_TRANSPOSE_MAX)
    {
        s_note_transpose = transpose;
    }
    s_note_channel = in[4] & 0x0F;

    s8 octave = (s8)in[5];

    if (octave >= NOTE_OCT_MIN && octave <= NOTE_OCT_MAX)
    {
        s_note_octave = octave;
    }
    s_dirty = 1;
}

u8 seq_ui_get_velocity(void)
{
    return s_current_velocity;
}

void seq_ui_set_velocity(u8 velocity)
{
    s_current_velocity = velocity & 0x7F;
    s_dirty = 1;
}

u8 seq_ui_selected_track(void)
{
    return s_selected_track;
}

void seq_ui_select_track(u8 track)
{
    if (track < SEQ_TRACK_COUNT)
    {
        s_selected_track = track;
    }
}

// ____________________________________________________________________________
//
// Capture
// ____________________________________________________________________________

static void capture_note(u8 note, u8 velocity)
{
    if (!s_capture_armed || !g_seq.playing)
    {
        return;
    }

    // land on the selected track's own grid, honouring its rate; capture
    // records into the playing pattern (or the next chained one when the
    // hit rounds forward past the pattern end)
    u8 pattern, step, offset;

    seq_engine_capture_pos(s_selected_track, s_capture_quantize,
                           &pattern, &step, &offset);
    seq_engine_capture_write(s_selected_track, pattern, step, note,
                             velocity, offset);
    s_dirty = 1;
}

void seq_ui_midi_event(u8 status, u8 d1, u8 d2)
{
    u8 type = status & 0xF0;
    u8 channel = status & 0x0F;

    // the control channel carries device-fader CC and user-pad LED
    // feedback - never pattern material
    if (channel == SEQ_CTRL_CHANNEL)
    {
        if (type == NOTEON || type == NOTEOFF)
        {
            if (seq_pad_to_grid(d1) != SEQ_NO_PAD)
            {
                s_user_led[d1] = (type == NOTEON) ? d2 : 0;

                if (s_mode == MODE_USER)
                {
                    paint_pad(d1);
                }
            }
        }
        else if (type == CC && d1 >= SEQ_FADER_CC_BASE
                            && d1 < SEQ_FADER_CC_BASE + 8)
        {
            u8 col = d1 - SEQ_FADER_CC_BASE;

            s_fader_cur[col] = d2;
            s_fader_target[col] = d2;

            if (s_mode == MODE_DEVICE)
            {
                repaint_fader_column(col);
            }
        }
        return;
    }

    // capture note-ons from any other channel; note-offs are ignored
    // (the sequencer imposes its own gate length)
    if (type == NOTEON && d2)
    {
        capture_note(d1, d2);
    }
}

void seq_ui_aftertouch_event(u8 index, u8 value)
{
    u8 grid = seq_pad_to_grid(index);

    if (grid == SEQ_NO_PAD)
    {
        return;
    }

    if (s_mode == MODE_NOTE)
    {
        if (s_note_aftertouch && s_note_held[grid] != PAD_NOTE_NONE)
        {
            seq_engine_send_aftertouch(s_note_held_ch[grid],
                                       s_note_held[grid], value);
        }
        return;
    }

    if (s_mode == MODE_SESSION)
    {
        // pressure on a held drum pad, in dynamic velocity mode only
        if (s_current_velocity)
        {
            return;
        }

        u8 drum = seq_pad_to_drum(index);

        if (drum != SEQ_NO_PAD && s_pad_note[drum] != PAD_NOTE_NONE)
        {
            seq_engine_send_aftertouch(s_pad_channel[drum],
                                       s_pad_note[drum], value);
        }
    }
}

// ____________________________________________________________________________
//
// Session mode grid events
// ____________________________________________________________________________

static void step_event(u8 step, u8 value)
{
    u32 bit = 1UL << step;

    if (value)
    {
        // held for now - toggled on release unless it gets edited first
        s_held_steps |= bit;
        s_held_modified &= ~bit;
        s_step_press_ms[step] = s_now_ms;
        s_held_velocity[step] =
            (value < SEQ_MIN_VELOCITY) ? SEQ_MIN_VELOCITY : value;
    }
    else
    {
        // a long hold is a peek at the step's settings - only quick,
        // unedited taps toggle. Each step is judged by ITS OWN hold
        // time, so a fast tap overlapping another held step still lands
        u16 held_for = (u16)(s_now_ms - s_step_press_ms[step]);

        if ((s_held_steps & bit) && !(s_held_modified & bit) &&
            held_for < SEQ_HOLD_MS)
        {
            // fixed mode uses the selected level; dynamic mode uses the
            // strength of the hit that punched the step in
            u8 velocity = s_current_velocity ? s_current_velocity
                                             : s_held_velocity[step];

            seq_engine_toggle_note(s_selected_track, step,
                                   g_seq.track[s_selected_track].cursor_note,
                                   velocity);
        }
        s_held_steps &= ~bit;
        s_held_modified &= ~bit;
    }
}

static void drum_event(u8 pad, u8 value)
{
    if (value)
    {
        s16 note = drum_pad_note(pad);

        if (note < 0)
        {
            return;
        }

        seq_track_t *track = &g_seq.track[s_selected_track];

        // hitting a pad selects that note lane and plays the note live
        track->cursor_note = (u8)note;

        // with Shift held, the selection is silent - nothing is sent
        if (s_shift_held)
        {
            return;
        }

        s_pad_note[pad] = (u8)note;
        s_pad_channel[pad] = track->channel;

        // fixed mode plays at the selected level; dynamic mode plays with
        // the strength of the hit
        u8 velocity;

        if (s_current_velocity)
        {
            velocity = s_current_velocity;
        }
        else
        {
            velocity = (value < SEQ_MIN_VELOCITY) ? SEQ_MIN_VELOCITY : value;
        }
        seq_engine_send_note(track->channel, (u8)note, velocity);

        // live step-record from the pads as well
        capture_note((u8)note, velocity);
    }
    else if (s_pad_note[pad] != PAD_NOTE_NONE)
    {
        seq_engine_send_note(s_pad_channel[pad], s_pad_note[pad], 0);
        s_pad_note[pad] = PAD_NOTE_NONE;
    }
}

static void vel_event(u8 level)
{
    u8 velocity = level_velocity(level);

    // while steps are held, the chosen level edits them instead of
    // (un)placing anything - adding the lane note where it is missing.
    // The selection (and fixed/dynamic mode) is left untouched.
    if (hold_edit_active())
    {
        u8 note = g_seq.track[s_selected_track].cursor_note;

        for (u8 step = 0; step < SEQ_STEP_COUNT; ++step)
        {
            if (s_held_steps & (1UL << step))
            {
                seq_engine_set_note(s_selected_track, step, note, velocity);
                s_held_modified |= (1UL << step);
            }
        }
        return;
    }

    // tapping the selected level again switches to dynamic (hit-strength)
    // mode; tapping any level switches back to fixed
    if (s_current_velocity == velocity)
    {
        s_current_velocity = 0;
    }
    else
    {
        s_current_velocity = velocity;
    }
}

static void session_grid_event(u8 index, u8 value)
{
    // releases always reach their owners - the overlay and hold-edit
    // views must never swallow them (stuck notes and stuck hold-edit
    // state otherwise)
    if (!value)
    {
        u8 rgrid = seq_pad_to_grid(index);

        // a pattern-pad release finishes the overlay's tap-select
        if (s_patpad != 0xFF && rgrid == (u8)(24 + s_patpad))
        {
            if (!s_patpad_done && track_overlay_active())
            {
                seq_engine_select_edit(s_selected_track, s_patpad);
            }
            s_patpad = 0xFF;
            return;
        }

        u8 rstep = seq_pad_to_step(index);

        if (rstep != SEQ_NO_STEP)
        {
            step_event(rstep, 0);
            return;
        }

        u8 rdrum = seq_pad_to_drum(index);

        if (rdrum != SEQ_NO_PAD)
        {
            drum_event(rdrum, 0);   // no-op unless the pad is sounding
        }
        return;
    }

    // track button held: the grid sets that track's settings
    if (track_overlay_active())
    {
        u8 pgrid = seq_pad_to_grid(index);

        // pattern pads: tap = view/edit that pattern, hold = chain toggle
        if (pgrid >= 24 && pgrid < 24 + SEQ_PATTERN_COUNT)
        {
            s_patpad = (u8)(pgrid - 24);
            s_patpad_ms = 0;
            s_patpad_done = 0;
            return;
        }

        u8 lstep = seq_pad_to_step(index);

        if (lstep != SEQ_NO_STEP)
        {
            seq_engine_set_length(s_selected_track, lstep + 1);
            return;
        }

        u8 grid = seq_pad_to_grid(index);

        if (grid < SEQ_RATE_COUNT)                  // row 1: rate
        {
            seq_engine_set_rate(s_selected_track, SEQ_RATES[grid]);
        }
        else if (grid < 24)                         // rows 2-3: channel
        {
            g_seq.track[s_selected_track].channel = (u8)(grid - 8);
        }
        else if (grid < 24 + SEQ_PATTERN_COUNT)     // row 4: chain
        {
            seq_engine_toggle_chain(s_selected_track, (u8)(grid - 24));
        }
        else if (grid >= 28 && grid < 32)           // row 4: gate
        {
            g_seq.track[s_selected_track].gate = (u8)(grid - 28 + 1);
        }
        return;
    }

    u8 step = seq_pad_to_step(index);

    if (step != SEQ_NO_STEP)
    {
        step_event(step, value);
        return;
    }

    u8 drum = seq_pad_to_drum(index);

    if (drum != SEQ_NO_PAD)
    {
        // while steps are held the drum pads set their probability
        if (hold_edit_active())
        {
            u8 note = g_seq.track[s_selected_track].cursor_note;

            for (u8 s = 0; s < SEQ_STEP_COUNT; ++s)
            {
                if (s_held_steps & (1UL << s))
                {
                    seq_engine_set_prob(s_selected_track, s, note,
                                        (u8)(drum + 1));
                    s_held_modified |= (1UL << s);
                }
            }
            return;
        }

        drum_event(drum, value);
        return;
    }

    u8 level = seq_pad_to_vel(index);

    if (level != SEQ_NO_PAD && value)
    {
        vel_event(level);
    }
}

// ____________________________________________________________________________
//
// Note / device / user mode grid events
// ____________________________________________________________________________

static void note_grid_event(u8 grid, u8 value)
{
    // releases always send their note-off, even with the settings page
    // open - a note held while Shift+Note opened it must not stick
    if (!value)
    {
        if (s_note_held[grid] != PAD_NOTE_NONE)
        {
            seq_engine_send_note(s_note_held_ch[grid], s_note_held[grid], 0);
            s_note_held[grid] = PAD_NOTE_NONE;
        }
        return;
    }

    if (note_setup_active())
    {
        note_setup_event(grid);
        return;
    }

    {
        s16 note = note_grid_note(grid);

        if (note < 0)
        {
            return;
        }

        // free playing on note mode's own channel: unlike the drum pads,
        // this does not change the sequencer's shown note lane
        s_note_held[grid] = (u8)note;
        s_note_held_ch[grid] = s_note_channel;

        u8 velocity = (value < SEQ_MIN_VELOCITY) ? SEQ_MIN_VELOCITY : value;
        seq_engine_send_note(s_note_channel, (u8)note, velocity);
        capture_note((u8)note, velocity);
    }
}

static void fader_grid_event(u8 grid, u8 velocity)
{
    u8 col = grid % 8;
    u8 row = grid / 8;                      // 0 = bottom = 0%

    s_fader_target[col] = (u8)((u16)row * 127 / 7);

    // harder press = faster glide (in CC units per update tick)
    s_fader_speed[col] = (u8)(1 + velocity / 6);
}

static void user_grid_event(u8 index, u8 value)
{
    if (value)
    {
        s_user_held[index] = 1;
        seq_engine_send_note(SEQ_CTRL_CHANNEL, index, value);
    }
    else
    {
        s_user_held[index] = 0;
        seq_engine_send_note(SEQ_CTRL_CHANNEL, index, 0);
    }
}

// ____________________________________________________________________________
//
// Surface dispatch
// ____________________________________________________________________________

// a mode switch takes the grid away from whatever the fingers were
// holding - their releases would land in the new mode's handler and be
// lost. Drop all transient held state and silence its live notes.
static void flush_holds(void)
{
    s_held_steps = 0;
    s_held_modified = 0;

    for (u8 pad = 0; pad < DRUM_PAD_COUNT; ++pad)
    {
        if (s_pad_note[pad] != PAD_NOTE_NONE)
        {
            seq_engine_send_note(s_pad_channel[pad], s_pad_note[pad], 0);
            s_pad_note[pad] = PAD_NOTE_NONE;
        }
    }
    for (u8 g = 0; g < 64; ++g)
    {
        if (s_note_held[g] != PAD_NOTE_NONE)
        {
            seq_engine_send_note(s_note_held_ch[g], s_note_held[g], 0);
            s_note_held[g] = PAD_NOTE_NONE;
        }
    }
    for (u8 i = 0; i < 100; ++i)
    {
        if (s_user_held[i])
        {
            seq_engine_send_note(SEQ_CTRL_CHANNEL, i, 0);
            s_user_held[i] = 0;
        }
    }

    // and abandon any pattern-pad hold in progress
    s_patpad = 0xFF;
    s_patpad_done = 0;
}

void seq_ui_surface_event(u8 index, u8 value)
{
    // any interaction may change what the surface shows
    s_dirty = 1;

    u8 grid = seq_pad_to_grid(index);

    if (grid != SEQ_NO_PAD)
    {
        switch (s_mode)
        {
            case MODE_SESSION:
                session_grid_event(index, value);
                break;

            case MODE_NOTE:
                note_grid_event(grid, value);
                break;

            case MODE_DEVICE:
                if (value)
                {
                    fader_grid_event(grid, value);
                }
                break;

            default:
                user_grid_event(index, value);
                break;
        }
        return;
    }

    u8 fill = seq_pad_to_fill(index);

    if (fill != SEQ_NO_PAD)
    {
        // track each rate button, so rolling between rates keeps the
        // fill "held" until the LAST finger lifts
        if (value)
        {
            s_fill_btns |= (u8)(1 << fill);
            seq_engine_fill(FILL_RATES[fill], 1);
        }
        else
        {
            s_fill_btns &= (u8)~(1 << fill);
            if (!s_fill_btns)
            {
                seq_engine_fill(FILL_RATES[fill], 0);
            }
        }
        return;
    }

    // track select: held, it turns the session grid into that track's
    // length/rate overlay, so both press and release matter
    if (index >= BTN_TRACK_BASE && index < BTN_TRACK_BASE + SEQ_TRACK_COUNT)
    {
        u8 t = index - BTN_TRACK_BASE;

        if (value)
        {
            seq_ui_select_track(t);
            s_track_held |= (u8)(1 << t);
        }
        else
        {
            s_track_held &= (u8)~(1 << t);
        }
        return;
    }

    // Delete: destructive, so it arms on press and fires after a hold
    if (index == BTN_CLEAR)
    {
        s_clear_held = value ? 1 : 0;
        s_clear_ms = 0;
        s_clear_done = 0;
        return;
    }

    if (index == BTN_SHIFT)
    {
        s_shift_held = value ? 1 : 0;
        return;
    }

    // mode buttons: tap = switch and stay; held long, the switch is
    // momentary and reverts on release (all four, Note included).
    // Shift+Note opens/closes the note settings page.
    if (index >= BTN_SESSION && index <= BTN_USER)
    {
        u8 mode = index - BTN_SESSION;      // enum order matches the row

        if (value)
        {
            if (index == BTN_NOTE && s_shift_held)
            {
                // Shift+Note: toggle the settings page, entering note
                // mode if needed; never part of a momentary hold
                if (s_mode == MODE_NOTE && s_note_setup_on)
                {
                    s_note_setup_on = 0;
                }
                else
                {
                    if (s_mode != MODE_NOTE)
                    {
                        flush_holds();
                    }
                    s_mode = MODE_NOTE;
                    s_note_setup_on = 1;
                }
                s_mode_btn = 0xFF;
            }
            else if (index == BTN_NOTE && s_mode == MODE_NOTE)
            {
                // inside note mode: a tap closes the settings page if
                // open, otherwise toggles aftertouch
                if (s_note_setup_on)
                {
                    s_note_setup_on = 0;
                }
                else
                {
                    s_note_aftertouch = !s_note_aftertouch;
                }
            }
            else if (mode != s_mode)
            {
                flush_holds();
                if (s_mode_btn == 0xFF)
                {
                    s_mode_return = s_mode;
                }
                s_mode = mode;
                s_mode_btn = index;
                s_mode_hold_ms = 0;
                s_note_setup_on = 0;        // mode switches close the page
            }
        }
        else if (index == s_mode_btn)
        {
            if (s_mode_hold_ms >= SEQ_MODE_HOLD_MS)
            {
                flush_holds();
                s_mode = s_mode_return;     // it was a momentary peek
            }
            s_mode_btn = 0xFF;
        }
        return;
    }

    // everything below acts on press only
    if (!value)
    {
        return;
    }

    if (index >= BTN_MUTE_BASE && index < BTN_MUTE_BASE + SEQ_TRACK_COUNT)
    {
        u8 t = index - BTN_MUTE_BASE;

        g_seq.track[t].muted = !g_seq.track[t].muted;
        return;
    }

    u8 solo = seq_pad_to_solo(index);

    if (solo != SEQ_NO_PAD)
    {
        g_seq.track[solo].solo = !g_seq.track[solo].solo;
        return;
    }

    switch (index)
    {
        case BTN_PLAY:
            seq_clock_toggle_play();
            break;

        case BTN_TEMPO_DOWN:
            // an external clock owns the tempo - don't silently edit the
            // BPM the internal clock would fall back to
            if (!seq_clock_is_external())
            {
                seq_clock_set_bpm(seq_clock_get_bpm() - SEQ_BPM_INCREMENT);
            }
            break;

        case BTN_TEMPO_UP:
            if (!seq_clock_is_external())
            {
                seq_clock_set_bpm(seq_clock_get_bpm() + SEQ_BPM_INCREMENT);
            }
            break;

        case BTN_BANK_UP:
        case BTN_BANK_DOWN:
        {
            s8 delta = (index == BTN_BANK_UP) ? 1 : -1;

            if (s_mode == MODE_NOTE)
            {
                s8 oct = s_note_octave + delta;

                if (oct >= NOTE_OCT_MIN && oct <= NOTE_OCT_MAX)
                {
                    s_note_octave = oct;
                }
            }
            // while steps are held these buttons nudge them off the grid
            // instead of switching the drum bank
            else if (hold_edit_active())
            {
                u8 note = g_seq.track[s_selected_track].cursor_note;

                for (u8 step = 0; step < SEQ_STEP_COUNT; ++step)
                {
                    if (s_held_steps & (1UL << step))
                    {
                        seq_engine_nudge_step(s_selected_track, step, note,
                                              delta);
                        s_held_modified |= (1UL << step);
                    }
                }
            }
            else if (delta > 0 &&
                     g_seq.track[s_selected_track].drum_bank < DRUM_BANK_MAX)
            {
                ++g_seq.track[s_selected_track].drum_bank;
            }
            else if (delta < 0 &&
                     g_seq.track[s_selected_track].drum_bank > DRUM_BANK_MIN)
            {
                --g_seq.track[s_selected_track].drum_bank;
            }
            break;
        }

        case BTN_QUANTIZE:
            if (s_capture_quantize == SEQ_QUANT_8TH)
            {
                s_capture_quantize = SEQ_QUANT_16TH;
            }
            else if (s_capture_quantize == SEQ_QUANT_16TH)
            {
                s_capture_quantize = SEQ_QUANT_32ND;
            }
            else
            {
                s_capture_quantize = SEQ_QUANT_8TH;
            }
            break;

        case BTN_RECORD:
            s_capture_armed = !s_capture_armed;
            if (s_capture_armed)
            {
                s_cap_track = s_selected_track;
                s_cap_playing = g_seq.playing;
                s_cap_wraps_base = seq_engine_track_wraps(s_selected_track);
            }
            break;

        case BTN_DOUBLE:
            if (s_shift_held)
            {
                seq_engine_copy_pattern(s_selected_track);
            }
            else
            {
                seq_engine_paste_pattern(s_selected_track);
            }
            break;

        case BTN_SWING_DOWN:
            if (seq_engine_swing())
            {
                seq_engine_set_swing(seq_engine_swing() - 1);
            }
            break;

        case BTN_SWING_UP:
            seq_engine_set_swing(seq_engine_swing() + 1);
            break;

        default:
            break;
    }
}
