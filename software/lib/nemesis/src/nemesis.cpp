//
// Library for the programmable Eurorack DSP Module NEMESIS
//
// nemesis.cpp
//
// Parts shared by all hardware revisions: audio patching, control binding
// and the codec. Revision specific code lives in nemesis_rev1/2.cpp.
//
// Copyright © 2021 Arev Imer if not mentioned elsewise
// released under GNU GPL v3
//

#include "nemesis.h"
#include "nemesis_tdm.h"
#include "nemesis_hw.h"
#include "nemesis_settings.h"
#include "nemesis_transport.h"
#include <EEPROM.h>
#include <Wire.h>

namespace nemesis
{
   AudioControlCS42448 codec;

   namespace
   {
      // the CS42448 runs in TDM mode: 8 channels of 32 bit slots (24 bit
      // audio); programs read and write the DMA buffers (nemesis_tdm.h)
      InputTDM tdmIn;    // first: its DMA interrupt starts the updates
      OutputTDM tdmOut;

      Program *active = nullptr;

      constexpr int MAX_PROGRAMS = 32;
      Program *programs[MAX_PROGRAMS];
      int numPrograms = 0;
      int current = -1;

      // last selected program, kept apart from the calibration data; only
      // stored once it ran for a while, so a crashing program can't cause a
      // boot loop
      constexpr int PROGRAM_EEPROM_ADDRESS = 1024;
      constexpr uint8_t PROGRAM_EEPROM_MAGIC = 0xA5;
      constexpr uint32_t PROGRAM_STORE_DELAY_MS = 3000;
      bool storePending = false;
      elapsedMillis sinceSelect;

      // only the running program is updated by the Audio library
      FLASHMEM void run(Program &program)
      {
         for (int i = 0; i < numPrograms; i++)
            programs[i]->setActive(programs[i] == &program);
      }
   }

   FLASHMEM void begin(Program &program)
   {
      Program *list[] = {&program};
      begin(list, 1);
   }

   FLASHMEM void begin(Program *const *list, int count)
   {
      numPrograms = min(count, MAX_PROGRAMS);
      for (int i = 0; i < numPrograms; i++)
         programs[i] = list[i];

      int stored = 0;
      if (EEPROM.read(PROGRAM_EEPROM_ADDRESS) == PROGRAM_EEPROM_MAGIC)
         stored = EEPROM.read(PROGRAM_EEPROM_ADDRESS + 1);
      current = (stored < numPrograms && !hw::forceFirstProgram()) ? stored : 0;

      settings::begin();
      transport::begin();
      active = programs[current];
      active->start();
      settings::load(*active);
      run(*active);
      hw::begin(*active);
   }

   FLASHMEM void printAudioStatus(Print &out)
   {
      out.printf("audio: block %d samples, round trip %d samples; output written late %lu of %lu blocks\n",
                 AUDIO_BLOCK_SAMPLES, 2 * AUDIO_BLOCK_SAMPLES, (unsigned long)tdm::lateWrites(),
                 (unsigned long)tdm::writes());
   }

   FLASHMEM bool selectProgram(int index)
   {
      if (index < 0 || index >= numPrograms)
         return false;
      if (index == current)
         return true;
      Program *next = programs[index];
      Program *previous = active;

      // free the previous program first, so the next one gets the internal
      // RAM; its settings are written before (its update() is still called,
      // it returns early without a DSP)
      if (previous && previous != next)
      {
         settings::flush(*previous);
         NEMESIS_TRACE("switch: stop %s\n", previous->name());
         AudioNoInterrupts();
         previous->stop();
         AudioInterrupts();
      }

      // with the audio update paused: building a program changes FAUST's
      // global MIDI tables, which the running program's update() reads
      NEMESIS_TRACE("switch: start %s\n", next->name());
      AudioNoInterrupts();
      bool started = next->start();
      NEMESIS_TRACE("switch: started %d\n", started);
      if (!started && previous)
         previous->start(); // keep the previous program running
      AudioInterrupts();
      if (!started)
         return false;
      settings::load(*next);

      for (int i = 0; i < NUM_LEDS; i++) // the LEDs belong to the program
         hw::setLed(i, false);
      AudioNoInterrupts();
      run(*next); // the old program is no longer updated
      active = next;
      current = index;
      AudioInterrupts();

      storePending = true;
      sinceSelect = 0;
      return true;
   }

   // in RAM1 (DTCM: single cycle, no cache); RAM2 stays free for the heap,
   // which FAUST's MIDI handling needs a lot of (8 kB per MIDI mapped parameter)
#ifndef NEMESIS_ARENA_SIZE
#define NEMESIS_ARENA_SIZE (192 * 1024)
#endif
   namespace
   {
      uint8_t arena[NEMESIS_ARENA_SIZE] __attribute__((aligned(32)));
      size_t arena_used = 0;
   }

   void *arenaAllocate(size_t size)
   {
      size_t start = (arena_used + 31) & ~(size_t)31;
      if (start + size > sizeof(arena))
         return nullptr;
      arena_used = start + size;
      return arena + start;
   }

   bool arenaOwns(const void *p)
   {
      return p >= (const void *)arena && p < (const void *)(arena + sizeof(arena));
   }

   void arenaReset() { arena_used = 0; }

   int currentProgram() { return current; }

   int programCount() { return numPrograms; }

   Program *program(int index)
   {
      return (index >= 0 && index < numPrograms) ? programs[index] : nullptr;
   }

   void update()
   {
      if (storePending && sinceSelect > PROGRAM_STORE_DELAY_MS)
      {
         storePending = false;
         EEPROM.write(PROGRAM_EEPROM_ADDRESS, PROGRAM_EEPROM_MAGIC);
         EEPROM.write(PROGRAM_EEPROM_ADDRESS + 1, current);
      }
      settings::service();
      hw::scan();
      if (active)
      {
         for (Control &c : active->controls())
         {
            float v;
            if (c.volts)
            {
               *c.zone = hw::inputVolts(c.input); // V/Oct: volts, no scaling or CV routing
               continue;
            }
            if (c.input >= 0)
               v = hw::input(c.input);
            else if (c.cvMask || c.changed) // settings are only written when needed, MIDI CCs may set them too
               v = c.position(c.value);
            else
               continue;
            c.changed = false;
            for (uint32_t mask = c.cvMask; mask; mask &= mask - 1)
               v += hw::input(__builtin_ctz(mask));
            v = constrain(v, 0.0f, 1.0f);
            *c.zone = c.gate ? (v > 0.5f ? 1.0f : 0.0f) : c.fromPosition(v);
         }
         for (const Indicator &i : active->indicators())
            if (i.led >= 0)
               hw::setLed(i.led, *i.zone > 0.5f * (i.min + i.max));
         hw::service(*active);
      }
   }

   float input(int index)
   {
      return (index >= 0 && index < NUM_INPUTS) ? hw::input(index) : 0.0f;
   }

   float rawInput(int index)
   {
      return (index >= 0 && index < NUM_INPUTS) ? hw::rawInput(index) : 0.0f;
   }

   const char *inputName(int index)
   {
      return (index >= 0 && index < NUM_INPUTS) ? hw::inputNames[index] : "";
   }

   FLASHMEM int inputIndex(const char *name)
   {
      // accept "CV X" as well as "CV_X"
      char normalized[16];
      size_t len = 0;
      for (; name[len] && len < sizeof(normalized) - 1; len++)
         normalized[len] = (name[len] == ' ') ? '_' : name[len];
      normalized[len] = '\0';

      for (int i = 0; i < NUM_INPUTS; i++)
      {
         if (strcmp(normalized, hw::inputNames[i]) == 0)
            return i;
      }
      return hw::aliasIndex(normalized);
   }

   void setLed(int index, bool on)
   {
      if (index >= 0 && index < NUM_LEDS)
         hw::setLed(index, on);
   }

   bool buttonPressed(int index)
   {
      return index >= 0 && index < NUM_BUTTONS && hw::buttonPressed(index);
   }

   FLASHMEM void printControls(Stream &out)
   {
      if (!active)
         return;
      out.printf("NEMESIS rev%d running %s%s\n", NEMESIS_HW_REV, active->name(),
                 sdCardPresent() ? "" : " (no SD card, settings are not kept)");
      for (const Control &c : active->controls())
      {
         char cv[40] = "";
         for (int i = 0; i < NUM_INPUTS; i++)
            if (c.cvMask & (1u << i))
               snprintf(cv + strlen(cv), sizeof(cv) - strlen(cv), " + %s", inputName(i));
         if (c.input >= 0)
            out.printf("  %-6s%-14s -> %s [%g .. %g]\n", inputName(c.input), cv, c.label, c.min, c.max);
         else
            out.printf("  set   %-14s    %s = %g [%g .. %g]\n", cv, c.label, c.value, c.min, c.max);
      }
   }

   FLASHMEM bool codecWrite(uint8_t reg, uint8_t val)
   {
      Wire.beginTransmission(0x48);
      Wire.write(reg);
      Wire.write(val);
      return Wire.endTransmission() == 0;
   }
}

// The std::string/std::vector code in FAUST's headers links libstdc++'s
// verbose terminate handler, which drags ~15 kB of name demangler into RAM1
// and has nowhere to print on the Teensy anyway.
namespace __gnu_cxx
{
   void __verbose_terminate_handler() { abort(); }
}
