// SPDX-License-Identifier: GPL-3.0-or-later
//
// Library for the programmable Eurorack DSP Module NEMESIS
//
// nemesis_remote.h - remote control over USB serial (software/tools/remote)
//
// printRemoteState() writes one line "JSON {...}": the hardware revision,
// the programs and the current one, the CPU load, the current program's
// controls (settings: the value set; knobs: their value now; label, name,
// range, step, menu, unit, section, visible, ...), the system settings,
// transport and audio. setProgramSetting() and setSystemSetting() change
// them as the menus do (saved after a pause).
//
// Copyright © 2021 Arev Imer if not mentioned elsewise
// released under GNU GPL v3 or later (see LICENSE)
//

#ifndef nemesis_remote_h
#define nemesis_remote_h

#include <Arduino.h>

namespace nemesis
{
   void printRemoteState(Print &out);
   // a program setting by its index in the controls; a knob's value stands
   // until the knob reaches it (as with presets)
   bool setProgramSetting(int index, float value);
   // a system setting by its name in the JSON ("tempo", "midiChannel", ...)
   bool setSystemSetting(const char *name, float value);
}

#endif
