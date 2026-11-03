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
#include "nemesis_hw.h"
#include <EEPROM.h>
#include <Wire.h>

namespace nemesis
{
   AudioControlCS42448 codec;

   namespace
   {
      // the CS42448 runs in TDM mode: 16 slots of 16 bit, every 24 bit
      // channel uses two consecutive slots (MSB, LSB)
      constexpr int MAX_CHANNELS = 8;

      AudioInputTDM tdmIn;
      AudioOutputTDM tdmOut;
      AudioConnection *cords[4 * MAX_CHANNELS];
      int numCords = 0;

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

      FLASHMEM void patch(Program &program)
      {
         for (int i = 0; i < numCords; i++)
            delete cords[i];
         numCords = 0;

         int inputs = min(program.numAudioInputs(), MAX_CHANNELS);
         for (int ch = 0; ch < inputs; ch++)
         {
            cords[numCords++] = new AudioConnection(tdmIn, 2 * ch, program, 2 * ch);
            cords[numCords++] = new AudioConnection(tdmIn, 2 * ch + 1, program, 2 * ch + 1);
         }
         int outputs = min(program.numAudioOutputs(), MAX_CHANNELS);
         for (int ch = 0; ch < outputs; ch++)
         {
            cords[numCords++] = new AudioConnection(program, 2 * ch, tdmOut, 2 * ch);
            cords[numCords++] = new AudioConnection(program, 2 * ch + 1, tdmOut, 2 * ch + 1);
         }
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

      active = programs[current];
      active->start();
      patch(*active);
      hw::begin(*active);
   }

   FLASHMEM bool selectProgram(int index)
   {
      if (index < 0 || index >= numPrograms)
         return false;
      if (index == current)
         return true;
      Program *next = programs[index];
      if (!next->start()) // allocates the DSP on first use, outside the audio interrupt
         return false;

      AudioNoInterrupts();
      patch(*next); // the old program is disconnected and no longer updated
      active = next;
      current = index;
      AudioInterrupts();

      storePending = true;
      sinceSelect = 0;
      return true;
   }

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
      hw::scan();
      if (active)
      {
         for (const Control &c : active->controls())
         {
            float v = hw::input(c.input);
            if (c.cv >= 0)
               v = constrain(v + hw::input(c.cv), 0.0f, 1.0f);
            *c.zone = c.gate ? (v > 0.5f ? 1.0f : 0.0f) : c.min + v * (c.max - c.min);
         }
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
      out.printf("NEMESIS rev%d running %s\n", NEMESIS_HW_REV, active->name());
      for (const Control &c : active->controls())
         out.printf("  %-6s%s%-5s -> %s [%g .. %g]\n", inputName(c.input), c.cv >= 0 ? " + " : "   ",
                    c.cv >= 0 ? inputName(c.cv) : "", c.label, c.min, c.max);
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
