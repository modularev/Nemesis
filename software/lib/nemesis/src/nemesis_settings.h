//
// Library for the programmable Eurorack DSP Module NEMESIS
//
// nemesis_settings.h
//
// Internal interface of the settings storage (nemesis_settings.cpp), the
// public part is in nemesis.h.
//
// Copyright © 2021 Arev Imer if not mentioned elsewise
// released under GNU GPL v3
//

#ifndef nemesis_settings_h
#define nemesis_settings_h

#include "nemesis.h"

namespace nemesis
{
   namespace settings
   {
      // mounts the SD card and reads the system settings
      // use_card false: the SD card stays untouched this session (safe start
      // after a hang)
      void begin(bool use_card = true);
      // reads the settings of a program the first time it is started
      void load(Program &program);
      // writes changed settings to the card, called from update()
      void service();
      // writes a program's pending changes now (before it is stopped)
      void flush(Program &program);
      // presets: a program's [action:preset] setting chooses a line of
      // /NEMESIS/<program>.PRE ("name|label=value|label=value", values as in
      // the menu, '#' comments). loadPresets() reads the names into the
      // setting's menu (after the settings, the stored choice isn't applied
      // again); service() loads a preset when the setting changes
      void loadPresets(Program &program);
      void servicePresets(Program &program);
   }
}

#endif
