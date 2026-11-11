//
// NEMESIS firmware with several programs
//
// The programs are listed in platformio.ini (custom_faust_programs);
// lib/faust_programs/nemesis_programs.h is generated from that list.
// On rev2 the encoder selects (turn) and loads (push) a program, the last
// one is restored at power up. Over USB serial: "PROG <n>" selects program n
// (1 based), "CONTROLS" lists its hardware bindings, "MEM" the PSRAM size,
// "GET /breaker/bpm" a parameter or meter of the running program, "STATUS" shows MIDI
// traffic, voices and NaN resets, "CAL" the CV and output calibration; the
// CPU load is reported every 2 s (with uptime, reset cause and chip
// temperature). Test commands: "SET <setting> <value>", "RANDOM ALL|<page>", "ROLL +|-",
// "MUTE"/"UNMUTE" (the program's DSP not computed), "SILENT"/"LOUD" (computed,
// outputs zeroed), "CLOCK <MHz>" (processor clock), "MIDICH <n>"
// (MIDI channel, not saved), "RATE <Hz>" (sample rate setting, 0 auto),
// "WRITE <path> <bytes>" + the bytes (a file on the SD card), "RAW"
// (control inputs unfiltered), "HANG" (loop() stops:
// the watchdog resets the module after 4 s).
//

#include <nemesis.h>
#include "nemesis_programs.h"
#include <nemesis_hw.h>

extern "C" uint8_t external_psram_size; // MB, set by the startup code
extern "C" char *__brkval;
extern "C" unsigned long _heap_end;
extern "C" uint32_t set_arm_clock(uint32_t frequency); // Teensy core, clockspeed.c
#include <malloc.h>
#include <SD.h>
static size_t heapTop() { return (char *)&_heap_end - __brkval; } // never used heap above the break

elapsedMillis sinceReport;
char command[64];
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
      else if (strncmp(command, "GET ", 4) == 0)
         Serial.printf("%s = %g\n", command + 4, nemesis::program(nemesis::currentProgram())->getParamValue(command + 4));
      else if (strncmp(command, "RANDOM", 6) == 0)
      {
         // RANDOM ALL / RANDOM <page>: as the randomize setting (testing)
         nemesis::Program *p = nemesis::program(nemesis::currentProgram());
         int page = strcmp(command + 6, " ALL") == 0 ? -1 : atoi(command + 6);
         p->randomize(page);
         nemesis::programSettingsChanged(*p);
         Serial.printf("OK RANDOM %d\n", page);
      }
      else if (strncmp(command, "SET ", 4) == 0)
      {
         // SET <setting label> <value>: as if set in the menu (testing)
         char *space = strrchr(command + 4, ' ');
         nemesis::Program *p = nemesis::program(nemesis::currentProgram());
         int index = -1;
         if (space)
         {
            *space = '\0';
            index = p->controlIndex(command + 4);
         }
         if (index >= 0)
         {
            nemesis::Control &c = p->controls()[index];
            c.value = atof(space + 1);
            c.changed = true;
            nemesis::programSettingsChanged(*p);
            Serial.printf("OK SET %s %g\n", command + 4, c.value);
         }
         else
            Serial.printf("? SET %s\n", command + 4);
      }
      else if (strcmp(command, "ROLL +") == 0 || strcmp(command, "ROLL -") == 0)
      {
         // as turning the roll setting one detent (testing)
         nemesis::Program *p = nemesis::program(nemesis::currentProgram());
         if (command[5] == '+')
            p->roll(p->rollTarget());
         else
            p->rollBack();
         nemesis::programSettingsChanged(*p);
         Serial.printf("OK ROLL #%d\n", p->rollPosition());
      }
      else if (strncmp(command, "CLOCK ", 6) == 0)
      {
         // test: processor clock in MHz (the program's cpu_mhz applies again on a switch)
         uint32_t mhz = atoi(command + 6);
         Serial.printf("CLOCK %lu -> %lu MHz\n", (unsigned long)(F_CPU_ACTUAL / 1000000), (unsigned long)mhz);
         Serial.flush();
         delay(50);
         uint32_t got = set_arm_clock(mhz * 1000000);
         Serial.printf("OK CLOCK %lu MHz\n", (unsigned long)(got / 1000000));
      }
      else if (strncmp(command, "WRITE ", 6) == 0)
      {
         // WRITE <path> <bytes>, then the bytes: a file on the SD card (e.g.
         // presets, /NEMESIS/juno.PRE); 10 s without data ends it
         char path[48];
         unsigned long size = 0;
         if (sscanf(command + 6, "%47s %lu", path, &size) != 2 || !nemesis::sdCardPresent() || size > 1000000)
         {
            Serial.println("? WRITE <path> <bytes> (SD card needed)");
            continue;
         }
         SD.remove(path);
         File file = SD.open(path, FILE_WRITE);
         Serial.printf("READY %lu\n", size);
         unsigned long got = 0;
         elapsedMillis idle;
         uint8_t chunk[256];
         while (got < size && idle < 10000)
         {
            nemesis::watchdogFeed();
            int n = Serial.available();
            if (n <= 0)
               continue;
            n = Serial.readBytes((char *)chunk, min((unsigned long)n, min((unsigned long)sizeof(chunk), size - got)));
            if (file)
               file.write(chunk, n);
            got += n;
            idle = 0;
         }
         bool ok = file && got == size;
         if (file)
            file.close();
         Serial.printf("%s WRITE %s %lu of %lu bytes\n", ok ? "OK" : "FAILED", path, got, size);
      }
      else if (strncmp(command, "RATE ", 5) == 0)
      {
         // test: system setting sample rate (0 auto, 48000, 96000), not saved
         nemesis::systemSettings().sampleRate = atoi(command + 5);
         Serial.printf("OK RATE %d\n", nemesis::systemSettings().sampleRate);
      }
      else if (strncmp(command, "MIDICH ", 7) == 0)
      {
         // test: the MIDI channel (0 omni), not saved
         nemesis::systemSettings().midiChannel = constrain(atoi(command + 7), 0, 16);
         Serial.printf("OK MIDICH %d\n", nemesis::systemSettings().midiChannel);
      }
      else if (strcmp(command, "RAW") == 0)
      {
         // test: all control inputs unfiltered (0..1), for logging at a high rate
         Serial.printf("RAW %lu", millis());
         for (int i = 0; i < nemesis::NUM_INPUTS; i++)
            Serial.printf(" %.4f", nemesis::hw::rawInput(i));
         Serial.printf(" %.1f\n", tempmonGetTemp());
      }
      else if (strcmp(command, "HANG") == 0)
      {
         // test of the watchdog: loop() stops, the module resets after 4 s
         Serial.println("OK HANG");
         Serial.flush();
         while (true)
         {
         }
      }
      else if (strcmp(command, "SILENT") == 0 || strcmp(command, "LOUD") == 0)
      {
         nemesis::debugSilent = command[0] == 'S';
         Serial.printf("OK %s\n", command);
      }
      else if (strcmp(command, "MUTE") == 0 || strcmp(command, "UNMUTE") == 0)
      {
         nemesis::debugMute = command[0] == 'M';
         Serial.printf("OK %s\n", command);
      }
      else if (strcmp(command, "MEM") == 0)
         Serial.printf("PSRAM %u MB, heap free %u kB (top %u kB)\n", external_psram_size,
                       (unsigned)(heapTop() + mallinfo().fordblks) / 1024, (unsigned)heapTop() / 1024);
#if NEMESIS_HW_REV == 2
      else if (strcmp(command, "CAL") == 0)
         nemesis::printVoltCalibration(Serial);
#endif
      else if (strcmp(command, "STATUS") == 0)
      {
         nemesis::program(nemesis::currentProgram())->printStatus(Serial);
         nemesis::printAudioStatus(Serial);
      }
   }
}

// why the last reset happened (SRC_SRSR, read before anything else)
static uint32_t resetCause = 0;

void setup()
{
   resetCause = SRC_SRSR;
   SRC_SRSR = resetCause; // write 1 to clear
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
      Serial.printf("PROGRAM %d %s  CPU %.1f%% (max %.1f%%)  audio blocks %d (max %d)  up %lu ms, reset 0x%lx, %.1f C\n",
                    nemesis::currentProgram() + 1, nemesis::program(nemesis::currentProgram())->name(),
                    nemesis::cpuLoad(), nemesis::cpuLoadMax(), AudioMemoryUsage(), AudioMemoryUsageMax(),
                    (unsigned long)millis(), (unsigned long)resetCause, tempmonGetTemp());
      AudioProcessorUsageMaxReset();
   }
}
