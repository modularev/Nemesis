/************************** BEGIN nemesis-serial-midi.h **************************
 FAUST Architecture File
 Copyright (C) 2019-2020 GRAME, Centre National de Creation Musicale
 Copyright (C) 2021 Arev Imer
 ---------------------------------------------------------------------
 This program is free software; you can redistribute it and/or modify
 it under the terms of the GNU Lesser General Public License as published by
 the Free Software Foundation; either version 2.1 of the License, or
 (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU Lesser General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA.

 EXCEPTION : As a special exception, you may create a larger work
 that contains this FAUST architecture section and distribute
 that work under terms of your choice, so long as this FAUST
 architecture section is not modified.
 ***************************************************************************/

#ifndef __nemesis_serial_midi__
#define __nemesis_serial_midi__

#include <cstdlib>

#include "Arduino.h"
#include "faust/midi/midi.h"

#include <nemesis_program.h> // midiRead(), midiChannel()

/**
 *  MIDI input for the NEMESIS module. The library reads the MIDI jack and
 *  USB MIDI in the background (nemesis_transport.cpp) and keeps the MIDI
 *  clock for its transport; this handler passes the queued messages to
 *  FAUST, filtered by the MIDI channel setting.
 */
class nemesis_midi : public midi_handler {

    private:

        void handleMessage(uint8_t status, uint8_t data1, uint8_t data2)
        {
            double time = 0.;
            int channel = status & 0x0F; // midi_handler expects 0-15
            int listen = nemesis::midiChannel();
            if (listen > 0 && channel != listen - 1) return;
            switch (status & 0xF0) {
                case 0x80:
                    handleKeyOff(time, channel, data1, data2);
                    break;
                case 0x90:
                    handleKeyOn(time, channel, data1, data2);
                    break;
                case 0xA0:
                    handlePolyAfterTouch(time, channel, data1, data2);
                    break;
                case 0xB0:
                    handleCtrlChange(time, channel, data1, data2);
                    break;
                case 0xC0:
                    handleProgChange(time, channel, data1);
                    break;
                case 0xD0:
                    handleAfterTouch(time, channel, data1);
                    break;
                case 0xE0:
                    handlePitchWheel(time, channel, data1, data2);
                    break;
            }
        }

        void handleRealtime(uint8_t status)
        {
            double time = 0.;
            switch (status) {
                case 0xF8:
                    handleClock(time);
                    break;
                case 0xFA:  // Start
                case 0xFB:  // Continue
                    handleStart(time);
                    break;
                case 0xFC:
                    handleStop(time);
                    break;
            }
        }

    public:

        nemesis_midi() : midi_handler("nemesis") {}

        virtual ~nemesis_midi() { stopMidi(); }

        bool startMidi() { return true; } // the library opened the MIDI jack

        void stopMidi() {}

        // a message from the library's queue, before every audio block
        void handle(const nemesis::MidiMessage& m)
        {
            if (m.status >= 0xF8) {
                handleRealtime(m.status);
            } else {
                handleMessage(m.status, m.data1, m.data2);
            }
        }
};

#endif
/**************************  END  nemesis-serial-midi.h **************************/
