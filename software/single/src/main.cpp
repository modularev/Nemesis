//
// NEMESIS firmware with a single program
//
// The program is chosen in platformio.ini (custom_faust_programs). Its
// parameters are bound to the hardware by their label metadata, see
// ../programs/README.md. Over USB serial: "CONTROLS" lists the bindings,
// "STATUS" shows MIDI traffic; the CPU load is reported every 2 s.
//

#include <nemesis.h>
#include "nemesis_programs.h"

elapsedMillis sinceReport;
char command[24];
size_t commandLength = 0;

void setup()
{
   if (CrashReport)
   {
      while (!Serial && millis() < 4000)
      {
      }
      Serial.print(CrashReport);
   }
   nemesis::begin(nemesis_programs);
   nemesis::printControls();
}

void loop()
{
   nemesis::update();

   while (Serial.available())
   {
      char c = Serial.read();
      if (c != '\n' && c != '\r')
      {
         if (commandLength < sizeof(command) - 1)
            command[commandLength++] = c;
         continue;
      }
      command[commandLength] = '\0';
      commandLength = 0;
      if (strcmp(command, "CONTROLS") == 0)
         nemesis::printControls();
      else if (strcmp(command, "STATUS") == 0)
         nemesis::program(0)->printStatus(Serial);
   }

   if (sinceReport >= 2000)
   {
      sinceReport = 0;
      Serial.printf("CPU %.1f%% (max %.1f%%)  audio blocks %d (max %d)\n",
                    AudioProcessorUsage(), AudioProcessorUsageMax(), AudioMemoryUsage(), AudioMemoryUsageMax());
      AudioProcessorUsageMaxReset();
   }
}
