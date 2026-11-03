//
// mooSpace - variable space reverb for the NEMESIS module
//
// The FAUST program src/mooSpace.dsp is compiled to lib/faust_programs/mooSpace.h
// at build time. Its parameters are bound to the hardware inputs by their
// [hw:..] / [rev1:..] / [rev2:..] label metadata, see the .dsp file.
//

#include <nemesis.h>
#include "mooSpace.h"

mooSpace reverb;

void setup()
{
   if (CrashReport)
   {
      while (!Serial && millis() < 4000)
      {
      }
      Serial.print(CrashReport);
   }
   nemesis::begin(reverb);
   nemesis::printControls();
}

void loop()
{
   nemesis::update();
}
