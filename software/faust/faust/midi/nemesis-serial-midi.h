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

namespace nemesis {
    int midiChannel(); // system setting: 0 omni, 1..16
}

/**
 *  MIDI input for the NEMESIS module: serial MIDI on Serial1 and, when the
 *  Teensy USB type includes MIDI, USB MIDI.
 *
 *  Serial MIDI is parsed here instead of using the Arduino MIDI library,
 *  whose 'midi' namespace collides with FAUST's 'midi' class.
 */
class nemesis_midi : public midi_handler {

    private:

        uint8_t fStatus = 0;
        uint8_t fData[2];
        int fDataCount = 0;

        static int dataLength(uint8_t status)
        {
            switch (status & 0xF0) {
                case 0xC0:  // Program Change
                case 0xD0:  // Channel Pressure
                    return 1;
                default:
                    return 2;
            }
        }

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

        // running status parser, system common and SysEx messages are skipped
        void parse(uint8_t byte)
        {
            if (byte >= 0xF8) {
                handleRealtime(byte);
            } else if (byte >= 0xF0) {
                fStatus = 0;
            } else if (byte & 0x80) {
                fStatus = byte;
                fDataCount = 0;
            } else if (fStatus) {
                fData[fDataCount++] = byte;
                if (fDataCount == dataLength(fStatus)) {
                    handleMessage(fStatus, fData[0], fDataCount > 1 ? fData[1] : 0);
                    fDataCount = 0;
                }
            }
        }

    public:

        // traffic counters for diagnostics (shared by all programs)
        inline static uint32_t fSerialBytes = 0;
        inline static uint32_t fUSBMessages = 0;
        inline static uint8_t fLastStatus = 0;

        nemesis_midi() : midi_handler("nemesis") {}

        virtual ~nemesis_midi() { stopMidi(); }

        bool startMidi()
        {
            Serial1.begin(31250);
            return true;
        }

        void stopMidi() {}

        void processMidi()
        {
            while (Serial1.available()) {
                uint8_t byte = Serial1.read();
                fSerialBytes++;
                if (byte & 0x80) fLastStatus = byte;
                parse(byte);
            }
#if defined(MIDI_INTERFACE)
            while (usbMIDI.read()) {
                fUSBMessages++;
                uint8_t type = usbMIDI.getType();
                if (type >= 0xF8) {
                    handleRealtime(type);
                } else if (type >= 0x80 && type < 0xF0) {
                    handleMessage(type | (usbMIDI.getChannel() - 1), usbMIDI.getData1(), usbMIDI.getData2());
                }
            }
#endif
        }
};

#endif
/**************************  END  nemesis-serial-midi.h **************************/
