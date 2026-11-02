#include "app.h"
#include "config.h"
#include "clock.h"
#include "engine.h"

static u16 s_bpm;
static u32 s_period_us;     // microseconds per MIDI clock tick
static u32 s_accum_us;

// countdown since the last external clock pulse; nonzero = an external
// clock is present and owns the transport timing
static u16 s_ext_timeout_ms;

// external pulses counted since the master's START (or since sync began),
// so the master's bar phase is known even while we're stopped
static u32 s_ext_count;

// last received Song Position Pointer, in MIDI beats (16ths), and
// whether one arrived since the last START/CONTINUE (a plain
// STOP/CONTINUE pair must resume in place, not seek)
static u16 s_spp;
static u8 s_spp_fresh;

// bar-quantized start pending (slaved only)
static u8 s_armed;

static void update_period(void)
{
    s_period_us = 60000000UL / ((u32)s_bpm * SEQ_PPQN);
}

static void send_realtime(u8 status)
{
#if SEQ_SEND_USB
    hal_send_midi(SEQ_USB_PORT, status, 0, 0);
#endif
#if SEQ_SEND_DIN
    hal_send_midi(DINMIDI, status, 0, 0);
#endif
}

void seq_clock_init(void)
{
    s_bpm = SEQ_DEFAULT_BPM;
    s_accum_us = 0;
    s_ext_timeout_ms = 0;
    s_ext_count = 0;
    s_spp = 0;
    s_spp_fresh = 0;
    s_armed = 0;
    update_period();
}

// halt everything the clock owns without a play-button press: used by the
// total reset. Sends MIDI STOP when we were driving the transport, and
// cancels any pending bar-quantized start.
void seq_clock_halt(void)
{
    if (g_seq.playing && !s_ext_timeout_ms)
    {
        send_realtime(MIDISTOP);
    }
    s_armed = 0;
    s_accum_us = 0;
}

void seq_clock_ms(void)
{
    if (s_ext_timeout_ms)
    {
        if (--s_ext_timeout_ms == 0)
        {
            // external clock lost - the internal clock takes over
            // seamlessly at the set BPM from the next tick
            s_accum_us = 0;
            s_armed = 0;
        }
        return;
    }

    if (!g_seq.playing)
    {
        return;
    }

    s_accum_us += 1000;
    while (s_accum_us >= s_period_us)
    {
        s_accum_us -= s_period_us;
        send_realtime(MIDITIMINGCLOCK);
        seq_engine_clock_tick();
    }
}

void seq_clock_toggle_play(void)
{
    if (s_armed)
    {
        s_armed = 0;                // cancel the pending start
        return;
    }

    if (g_seq.playing)
    {
        seq_engine_stop();

        // don't drive other transports while we're slaved ourselves
        if (!s_ext_timeout_ms)
        {
            send_realtime(MIDISTOP);
        }
    }
    else if (s_ext_timeout_ms)
    {
        // slaved: start on the master's next bar line
        s_armed = 1;
    }
    else
    {
        s_accum_us = 0;
        seq_engine_start();
        send_realtime(MIDISTART);
    }
}

void seq_clock_set_bpm(u16 bpm)
{
    if (bpm < SEQ_MIN_BPM)
    {
        bpm = SEQ_MIN_BPM;
    }
    if (bpm > SEQ_MAX_BPM)
    {
        bpm = SEQ_MAX_BPM;
    }
    s_bpm = bpm;
    update_period();
}

u16 seq_clock_get_bpm(void)
{
    return s_bpm;
}

u8 seq_clock_is_external(void)
{
    return s_ext_timeout_ms != 0;
}

u8 seq_clock_is_armed(void)
{
    return s_armed;
}

void seq_clock_midi_in(u8 status, u8 d1, u8 d2)
{
    switch (status)
    {
        case MIDITIMINGCLOCK:
            // any steady clock stream claims the transport - no START
            // required, so clock-only masters work: press play and
            // follow their tempo
            s_ext_timeout_ms = SEQ_EXT_CLOCK_TIMEOUT_MS;

            // a pending quantized start fires on the master's "1"
            if (s_armed && (s_ext_count % SEQ_BAR_TICKS) == 0)
            {
                s_armed = 0;
                seq_engine_start();
            }

            if (g_seq.playing)
            {
                seq_engine_clock_tick();
            }
            ++s_ext_count;
            break;

        case MIDISTART:
            s_ext_timeout_ms = SEQ_EXT_CLOCK_TIMEOUT_MS;
            s_ext_count = 0;
            s_spp = 0;
            s_spp_fresh = 0;
            s_armed = 0;
            if (g_seq.playing)
            {
                seq_engine_stop();
            }
            seq_engine_start();
            break;

        case MIDICONTINUE:
            s_ext_timeout_ms = SEQ_EXT_CLOCK_TIMEOUT_MS;
            s_armed = 0;

            // a plain STOP/CONTINUE pair means resume in place; only an
            // SPP received since then repositions us
            if (s_spp_fresh)
            {
                s_spp_fresh = 0;
                seq_engine_seek((u32)s_spp * (SEQ_PPQN / 4));
                s_ext_count = (u32)s_spp * (SEQ_PPQN / 4);
            }
            seq_engine_resume();
            break;

        case MIDISTOP:
            if (g_seq.playing)
            {
                seq_engine_pause();     // freeze in place - CONTINUE resumes
            }
            s_armed = 0;
            break;

        case SONGPOSITIONPOINTER:
            s_spp = (u16)d1 | ((u16)d2 << 7);
            s_spp_fresh = 1;
            break;

        default:
            break;
    }
}
