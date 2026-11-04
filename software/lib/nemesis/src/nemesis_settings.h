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
      void begin();
      // reads the settings of a program the first time it is started
      void load(Program &program);
      // writes changed settings to the card, called from update()
      void service();
      // writes a program's pending changes now (before it is stopped)
      void flush(Program &program);
   }
}

#endif
