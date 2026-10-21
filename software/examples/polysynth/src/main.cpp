//
// polysynth - oscillator experiments for the NEMESIS module
//
// Every src/*.dsp is compiled to lib/faust_programs/<name>.h at build time,
// park programs you don't use by renaming them to .bdsp. Parameters are
// bound to the hardware by their label prefix, e.g. "CV_X: Pitch".
//

#include <nemesis.h>
#include "superosc.h"

superosc synth;

elapsedMillis sinceReport;

void setup()
{
   nemesis::begin(synth);
   nemesis::printControls();
}

void loop()
{
   nemesis::update();

   if (sinceReport >= 1000)
   {
      sinceReport = 0;
      Serial.printf("CPU %.1f%% (max %.1f%%), audio blocks %d (max %d)\n",
                    AudioProcessorUsage(), AudioProcessorUsageMax(),
                    AudioMemoryUsage(), AudioMemoryUsageMax());
   }
}
