// SPDX-License-Identifier: GPL-3.0-or-later
//
// NEMESIS firmware with several programs
//
// The programs are listed in platformio.ini (custom_faust_programs);
// lib/faust_programs/nemesis_programs.h is generated from that list.
// The last program is restored at power up (rev2: hold the encoder for the
// program list). Over USB serial: "PROG <n>" selects program n (1 based),
// "CONTROLS" lists its hardware bindings, "MEM" the PSRAM size and heap,
// "GET /breaker/bpm" a parameter or meter of the running program, "STATUS"
// shows MIDI traffic, voices and NaN resets, "CAL" the CV and output
// calibration; the CPU load is reported every 2 s (with uptime, reset cause
// and chip temperature). Remote control (software/tools/remote): "JSON",
// "PSET <index> <value>", "SYS <name> <value>", rev1 "KCAL [START|STORE|
// CANCEL|RESET]" (knob calibration: sweep each knob, store). Test commands: "SET
// <setting> <value>", "RANDOM ALL|<page>", "ROLL +|-", "MUTE"/"UNMUTE" (the
// program's DSP not computed), "SILENT"/"LOUD" (computed, outputs zeroed),
// "CLOCK <MHz>" (processor clock), "MIDICH <n>" (MIDI channel, not saved),
// "RATE <Hz>" (sample rate setting, 0 auto), "WRITE <path> <bytes>" + the
// bytes (a file on the SD card), "RAW" (control inputs unfiltered),
// "BLOCKS" (cycles of compute() of 4096 consecutive audio blocks: find slow
// single blocks), "CAPTURE <input> <samples>" / "CAPDUMP" (an input recorded
// from the DMA into PSRAM, then printed: patch an output into it), "MONITOR 0|1" (display updates off/on), "SCREEN" (the
// display's framebuffer as hex), "PSRAMTEST" (4 MB pattern written and read back 4 times: errors of the
// PSRAM bus), "HANG" (loop() stops: the watchdog resets the module after 4 s).
//

#include <nemesis.h>
#include "nemesis_programs.h"
#include <nemesis_hw.h>
#include <nemesis_remote.h>
#include <nemesis_tdm.h>
#include <nemesis_spectral.h>

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

// main loop only, rarely busy: from flash (ITCM room for the FFT, 2026-11-20)
FLASHMEM void readCommands()
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
#if NEMESIS_HW_REV == 2
      else if (sscanf(command, "MONITOR %d", &n) == 1)
      {
         // test: display updates on/off (1/0)
         nemesis::showMonitor(n != 0);
         Serial.printf("OK MONITOR %d\n", n != 0);
      }
#endif
      else if (strcmp(command, "BLOCKS") == 0)
      {
         // test: the cycles of compute() of 4096 consecutive audio blocks
         nemesis::startBlockLog();
         elapsedMillis waited;
         while (!nemesis::blockLogDone() && waited < 40000)
         {
            nemesis::watchdogFeed();
            delay(10);
         }
         nemesis::printBlockLog(Serial);
      }
      else if (strncmp(command, "CAPTURE ", 8) == 0)
      {
         // test: CAPTURE <input 1..8> <samples>: records that input from the
         // DMA into PSRAM (patch an output into it: clicks, levels)
         int input = 1, samples = 0;
         sscanf(command + 8, "%d %d", &input, &samples);
         nemesis::tdm::startCapture(input - 1, samples);
         Serial.printf("OK CAPTURE %d %d\n", input, samples);
      }
      else if (strcmp(command, "CAPDUMP") == 0)
      {
         // the captured samples as 24 bit values, 16 per line
         const int32_t *x;
         int n = nemesis::tdm::captured(x);
         Serial.printf("CAPDUMP %d %d\n", n, nemesis::sampleRate());
         for (int i = 0; i < n; i++)
         {
            Serial.print(x[i] >> 8);
            Serial.print((i % 16 == 15 || i == n - 1) ? '\n' : ' ');
            if (i % 4096 == 0)
               nemesis::watchdogFeed();
         }
         Serial.println("CAPDUMP end");
      }
      else if (strcmp(command, "SCREEN") == 0)
         nemesis::printScreen(Serial);
      else if (strcmp(command, "PSRAMTEST") == 0)
      {
         // a free PSRAM block filled with a pattern and read back from the
         // chip (cache cleared) several times: read/write errors of the bus
         const uint32_t words = 1u << 20; // 4 MB
         uint32_t *m = (uint32_t *)extmem_malloc(words * 4);
         if (!m || (uint32_t)m < 0x70000000u)
         {
            Serial.println("PSRAMTEST: no 4 MB PSRAM block free");
            free(m);
         }
         else
         {
            uint32_t x = micros() | 1, seed = x;
            for (uint32_t i = 0; i < words; i++)
            {
               x ^= x << 13; x ^= x >> 17; x ^= x << 5;
               m[i] = x;
            }
            arm_dcache_flush_delete(m, words * 4);
            uint32_t errors = 0, first = 0, firstGot = 0, firstWant = 0;
            elapsedMillis took;
            for (int pass = 0; pass < 4; pass++)
            {
               x = seed;
               for (uint32_t i = 0; i < words; i++)
               {
                  x ^= x << 13; x ^= x >> 17; x ^= x << 5;
                  if (m[i] != x)
                  {
                     if (!errors++)
                     {
                        first = i;
                        firstGot = m[i];
                        firstWant = x;
                     }
                  }
               }
               arm_dcache_delete(m, words * 4);
               nemesis::watchdogFeed();
            }
            Serial.printf("PSRAMTEST: %lu errors in 4 x 4 MB (%lu ms)", (unsigned long)errors, (unsigned long)(uint32_t)took);
            if (errors)
               Serial.printf(", first at word %lu: %08lx, wanted %08lx", (unsigned long)first, (unsigned long)firstGot, (unsigned long)firstWant);
            Serial.println();
            extmem_free(m);
         }
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
      else if (strcmp(command, "JSON") == 0)
      {
         // remote control (software/tools/remote): the state as one JSON line
         nemesis::printRemoteState(Serial);
      }
      else if (strncmp(command, "PSET ", 5) == 0)
      {
         // PSET <control index> <value>: a program setting
         int index;
         float value;
         if (sscanf(command + 5, "%d %f", &index, &value) == 2 && nemesis::setProgramSetting(index, value))
            Serial.printf("OK PSET %d\n", index);
         else
            Serial.printf("? PSET %s\n", command + 5);
      }
      else if (strncmp(command, "SYS ", 4) == 0)
      {
         // SYS <name> <value>: a system setting (names as in the JSON)
         char name[32];
         float value;
         if (sscanf(command + 4, "%31s %f", name, &value) == 2 && nemesis::setSystemSetting(name, value))
            Serial.printf("OK SYS %s\n", name);
         else
            Serial.printf("? SYS %s\n", command + 4);
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
      else if (strcmp(command, "SPECPROF") == 0)
      {
         // the spectral stage: average cycles per frame of each step
         uint32_t cyc[nemesis::spectral::PROFILE_STEPS], frames;
         nemesis::spectral::profile(cyc, frames);
         Serial.printf("SPECPROF %lu frames, %d points:", (unsigned long)frames, nemesis::spectral::size());
         uint32_t total = 0;
         for (int i = 0; i < nemesis::spectral::PROFILE_STEPS; i++)
         {
            Serial.printf(" %s %lu", nemesis::spectral::profileName[i], (unsigned long)cyc[i]);
            total += cyc[i];
         }
         Serial.printf("  total %lu\n", (unsigned long)total);
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
      {
         // the largest block malloc() can give now (binary search by trial)
         size_t lo = 0, hi = heapTop() + mallinfo().fordblks;
         while (hi - lo > 1024)
         {
            size_t mid = (lo + hi) / 2;
            void *p = malloc(mid);
            if (p)
               lo = mid;
            else
               hi = mid;
            free(p);
         }
         Serial.printf("PSRAM %u MB, heap free %u kB (top %u kB, largest block %u kB)\n", external_psram_size,
                       (unsigned)(heapTop() + mallinfo().fordblks) / 1024, (unsigned)heapTop() / 1024, (unsigned)lo / 1024);
      }
#if NEMESIS_HW_REV == 2
      else if (strcmp(command, "CAL") == 0)
         nemesis::printVoltCalibration(Serial);
#else
      else if (strcmp(command, "KCAL") == 0 || strncmp(command, "KCAL ", 5) == 0)
         nemesis::hw::knobCalibration(command[4] ? command + 5 : "", Serial);
#endif
      else if (strcmp(command, "STATUS") == 0)
      {
         nemesis::program(nemesis::currentProgram())->printStatus(Serial);
         nemesis::printAudioStatus(Serial);
         nemesis::printDisplayStats(Serial);
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
      nemesis::cpuLoadMaxReset();
   }
}
