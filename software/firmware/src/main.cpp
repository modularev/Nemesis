//
// NEMESIS firmware with several programs
//
// The programs are listed in platformio.ini (custom_faust_programs);
// lib/faust_programs/nemesis_programs.h is generated from that list.
// On rev2 the encoder selects (turn) and loads (push) a program, the last
// one is restored at power up. Over USB serial: "PROG <n>" selects program n
// (1 based), "CONTROLS" lists its hardware bindings, "STATUS" shows MIDI
// traffic, voices and NaN resets; the CPU load is reported every 2 s.
//

#include <nemesis.h>
#include "nemesis_programs.h"

elapsedMillis sinceReport;
char command[24];
size_t commandLength = 0;

void readCommands()
{
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
      int n;
      if (sscanf(command, "PROG %d", &n) == 1)
         Serial.printf("OK PROG %d %s\n", n, nemesis::selectProgram(n - 1) ? "selected" : "failed");
      else if (strcmp(command, "CONTROLS") == 0)
         nemesis::printControls();
      else if (strcmp(command, "STATUS") == 0)
         nemesis::program(nemesis::currentProgram())->printStatus(Serial);
   }
}

void setup()
{
   // before starting a program, in case that program is what crashed
   if (CrashReport)
   {
      while (!Serial && millis() < 4000)
      {
      }
      Serial.print(CrashReport);
   }
   nemesis::begin(nemesis_programs);
}

void loop()
{
   nemesis::update();
   readCommands();

   if (sinceReport >= 2000)
   {
      sinceReport = 0;
      Serial.printf("PROGRAM %d %s  CPU %.1f%% (max %.1f%%)  audio blocks %d (max %d)\n",
                    nemesis::currentProgram() + 1, nemesis::program(nemesis::currentProgram())->name(),
                    AudioProcessorUsage(), AudioProcessorUsageMax(), AudioMemoryUsage(), AudioMemoryUsageMax());
      AudioProcessorUsageMaxReset();
   }
}
