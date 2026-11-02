/******************************************************************************

 Copyright (c) 2015, Focusrite Audio Engineering Ltd.
 All rights reserved.

 Redistribution and use in source and binary forms, with or without
 modification, are permitted provided that the following conditions are met:

 * Redistributions of source code must retain the above copyright notice, this
 list of conditions and the following disclaimer.

 * Redistributions in binary form must reproduce the above copyright notice,
 this list of conditions and the following disclaimer in the documentation
 and/or other materials provided with the distribution.

 * Neither the name of Focusrite Audio Engineering Ltd., nor the names of its
 contributors may be used to endorse or promote products derived from
 this software without specific prior written permission.

 THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

 *****************************************************************************/

//______________________________________________________________________________
//
// LaunchStep - an XOX-style step sequencer for the Launchpad Pro Mk1,
// modelled on the Launchpad Pro Mk3's built-in sequencer.
//
// This file only wires the HAL callbacks to the sequencer modules in src/seq.
//______________________________________________________________________________

#include "app.h"

#include "seq/config.h"
#include "seq/clock.h"
#include "seq/engine.h"
#include "seq/store.h"
#include "seq/ui.h"

void app_init(const u16 *adc_raw)
{
    (void)adc_raw;

    // ui before store: the loaded state includes UI settings
    seq_clock_init();
    seq_engine_init();
    seq_ui_init();
    seq_store_load();
}

void app_timer_event()
{
    seq_clock_ms();
    seq_ui_ms();
}

void app_surface_event(u8 type, u8 index, u8 value)
{
    if (type == TYPESETUP)
    {
        seq_ui_setup_event(value);
        return;
    }

    seq_ui_surface_event(index, value);
}

void app_midi_event(u8 port, u8 status, u8 d1, u8 d2)
{
    (void)port;

    // realtime transport/clock/SPP from any input drives external sync
    seq_clock_midi_in(status, d1, d2);

    // channel messages feed capture mode when it is armed
    seq_ui_midi_event(status, d1, d2);
}

void app_aftertouch_event(u8 index, u8 value)
{
    seq_ui_aftertouch_event(index, value);
}

void app_sysex_event(u8 port, u8 *data, u16 count)
{
    // future: pattern dump/restore to a computer
    (void)port;
    (void)data;
    (void)count;
}

void app_cable_event(u8 type, u8 value)
{
    (void)type;
    (void)value;
}
