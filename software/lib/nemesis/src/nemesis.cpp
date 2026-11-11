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
#include "nemesis_loop.h"
#include "nemesis_transport.h"
#include <EEPROM.h>
#include <Wire.h>

namespace nemesis
{
   namespace
   {
      bool codec_ready = false; // hw::begin() set the codec up (applySampleRate())
      uint32_t unmute_at = 0;   // millis() when the codec's outputs come back after a rate change

      // waits (up to 30 ms) until the outputs faded out; a stopped program
      // can't fade, so the gain is set to 0 anyway
      void fadeOut()
      {
         outputTarget = 0.0f;
         for (uint32_t start = millis(); outputGain > 0.0f && millis() - start < 30;)
            delay(1);
         outputGain = 0.0f;
      }
      void fadeIn() { outputTarget = 1.0f; }
   }
   volatile float outputTarget = 1.0f;
   volatile float outputGain = 1.0f;

   uint32_t watchdog_cs = 0, watchdog_toval = 0; // as read back after watchdogBegin()

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
      // a program is remembered for the next power up only after running this
      // long: one that crashes soon after starting can't lock the module in
      constexpr uint32_t PROGRAM_STORE_DELAY_MS = 10000;
      bool storePending = false;
      elapsedMillis sinceSelect;

      // only the running program is updated by the Audio library
      // settings into the DSP before its first block (update() writes them
      // later, from loop()): a program may build state from them at once
      // (pollen: a frozen loop restored from the SD card)
      FLASHMEM void applySettings(Program &program)
      {
         for (Control &c : program.controls())
         {
            // settings, and knobs that still wait for their stored value
            if ((c.input >= 0 && !(c.knob && !c.caught)) || c.volts || c.edit)
               continue;
            float v = constrain(c.position(c.value), 0.0f, 1.0f);
            *c.zone = c.gate ? (v > 0.5f ? 1.0f : 0.0f) : c.fromPosition(v);
         }
      }

      FLASHMEM void run(Program &program)
      {
         for (int i = 0; i < numPrograms; i++)
            programs[i]->setActive(programs[i] == &program);
      }

      constexpr int DEFAULT_BLOCK_SIZE = 64;

      // the program's clock ('declare cpu_mhz', only lower than F_CPU),
      // set before it runs: strike's compute() from ITCM at 816 MHz drew
      // enough current to reset the module (reset cause 0x1, no crash)
      extern "C" uint32_t set_arm_clock(uint32_t frequency); // Teensy core, clockspeed.c
      void applyClock(const Program &program)
      {
         uint32_t wanted = F_CPU;
         if (program.cpuMHz() >= 150 && (uint32_t)program.cpuMHz() * 1000000 < F_CPU)
            wanted = (uint32_t)program.cpuMHz() * 1000000;
         if (wanted != F_CPU_ACTUAL)
         {
            set_arm_clock(wanted);
            Serial.printf("clock %lu MHz\n", (unsigned long)(F_CPU_ACTUAL / 1000000));
         }
      }

      // the block size of the setting, or the running program's (auto)
      void applyBlockSize()
      {
         int wanted = wantedBlockSize();
         if (wanted == tdm::blockSize())
            return;
         bool playing = outputTarget > 0.0f;
         if (playing)
            fadeOut();
         tdm::setBlockSize(wanted); // the audio stops for a moment
         if (playing)
            fadeIn();
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
      // safe start after starts that hung or crashed (counted in EEPROM):
      // program 1, SD card untouched
      bool safe = bootFailures() >= SAFE_AFTER_FAILURES;
      current = (stored < numPrograms && !hw::forceFirstProgram() && !safe) ? stored : 0;
      if (safe)
         Serial.printf("safe start: %d starts in a row ended early, SD card left alone\n", bootFailures());
      watchdogBegin();

      tdm::begin(); // MCLK for both sample rates, before the codec starts
      settings::begin(!safe);
      transport::begin();
      active = programs[current];
      active->start();
      settings::load(*active);
      applySettings(*active);
      settings::loadPresets(*active);
      loop::restore(*active);
      applyClock(*active);
      run(*active);
      hw::begin(*active);
      codec_ready = true;
      applyBlockSize();
   }

   int blockSize() { return tdm::blockSize(); }

   int wantedBlockSize()
   {
      int setting = systemSettings().blockSize;
      if (setting)
         return setting;
      int hint = active ? active->blockSizeHint() : 0;
      return (hint == 32 || hint == 64 || hint == 128) ? hint : DEFAULT_BLOCK_SIZE;
   }

   // the Audio library's figures assume AUDIO_BLOCK_SAMPLES at AUDIO_SAMPLE_RATE_EXACT
   float cpuLoad() { return AudioProcessorUsage() * AUDIO_BLOCK_SAMPLES / tdm::blockSize() * tdm::sampleRate() / AUDIO_SAMPLE_RATE_EXACT; }
   float cpuLoadMax() { return AudioProcessorUsageMax() * AUDIO_BLOCK_SAMPLES / tdm::blockSize() * tdm::sampleRate() / AUDIO_SAMPLE_RATE_EXACT; }

   int sampleRate() { return tdm::sampleRate(); }

   int wantedSampleRate(const Program *program)
   {
      int setting = systemSettings().sampleRate;
      if (setting == 48000 || setting == 96000)
         return setting;
      if (!program)
         program = active;
      int hint = program ? program->sampleRateHint() : 0;
      return (hint == 48000 || hint == 96000) ? hint : 48000;
   }

   uint8_t codecFunctionalMode()
   {
      // slave mode, speed detected from MCLK / LRCK (1024: single, 512:
      // double speed), MFREQ 100: MCLK 49.152 MHz at both rates (nemesis_tdm.cpp)
      return 0xF8 | 0x01;
   }

   FLASHMEM int applySampleRate(const Program &program)
   {
      int wanted = wantedSampleRate(&program);
      if (wanted == tdm::sampleRate())
         return wanted;
      // MCLK and the codec's settings stay (nemesis_tdm.cpp): only LRCK and
      // the bit clock change; the DACs muted while the codec follows
      // (unmuted ~30 ms after, in update())
      if (codec_ready)
      {
         codecWrite(0x07, 0xFF);
         unmute_at = millis() + 30;
      }
      tdm::setSampleRate(wanted);
      Serial.printf("sample rate %d\n", tdm::sampleRate());
      return tdm::sampleRate();
   }

   FLASHMEM void printAudioStatus(Print &out)
   {
      out.printf("SD: %s; failed starts before this one: %d\n", sdProblem() ? sdProblem() : "in use", bootFailures());
      out.printf("watchdog: CS %08lx TOVAL %lu\n", (unsigned long)watchdog_cs, (unsigned long)watchdog_toval);
      out.printf("audio: block %d samples, round trip %d samples; output written late %lu of %lu blocks\n",
                 tdm::blockSize(), 2 * tdm::blockSize(), (unsigned long)tdm::lateWrites(),
                 (unsigned long)tdm::writes());
   }

   namespace
   {
      bool restarting = false; // the running program starts again (sample rate setting)
   }

   FLASHMEM bool selectProgram(int index)
   {
      if (index < 0 || index >= numPrograms)
         return false;
      if (index == current && !restarting)
         return true;
      Program *next = programs[index];
      Program *previous = active;

      // free the previous program first, so the next one gets the internal
      // RAM; its settings are written before (its update() is still called,
      // it returns early without a DSP)
      fadeOut(); // no click from the old program stopping
      if (previous && (previous != next || restarting))
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
      {
         fadeIn();
         return false;
      }
      NEMESIS_TRACE("switch: load settings\n");
      settings::load(*next);
      NEMESIS_TRACE("switch: apply\n");
      applySettings(*next);
      settings::loadPresets(*next);
      NEMESIS_TRACE("switch: loop restore\n");
      loop::restore(*next); // before the program's first block
      NEMESIS_TRACE("switch: run\n");

      for (int i = 0; i < NUM_LEDS; i++) // the LEDs belong to the program
         hw::setLed(i, false);
      applyClock(*next);
      AudioNoInterrupts();
      run(*next); // the old program is no longer updated
      NEMESIS_TRACE("switch: block size\n");
      applyBlockSize();
      NEMESIS_TRACE("switch: done\n");
      active = next;
      current = index;
      AudioInterrupts();
      fadeIn();

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

   namespace
   {
      constexpr float CATCH_RANGE = 0.02f;      // a knob this close to its stored value takes over
      constexpr float KNOB_STORE_STEP = 0.004f; // knob moves that change the stored value
      constexpr float RELATIVE_STEP = 0.01f;    // button 1 + edit knob: movement that starts a relative edit
      constexpr uint32_t BUTTON_DEBOUNCE_MS = 30;

      // the knob reaches the stored position, or passed it since the last reading
      bool catches(Control &c, float pos, float stored)
      {
         bool reached = fabsf(pos - stored) < CATCH_RANGE ||
                        (c.catchRef >= 0.0f && (pos - stored) * (c.catchRef - stored) < 0.0f);
         c.catchRef = pos;
         return reached;
      }

      // button 1, debounced; used: a knob was turned while it was held
      bool button_one = false;
      bool button_one_used = false;
      elapsedMillis button_one_change;

      bool buttonOne()
      {
         bool now = hw::buttonPressed(0);
         if (now != button_one && button_one_change > BUTTON_DEBOUNCE_MS)
         {
            button_one = now;
            button_one_change = 0;
            if (now)
               button_one_used = false;
            else if (!button_one_used && active && active->pagerIndex() >= 0)
            {
               // released without turning a knob: the next page
               active->nextPage();
               programSettingsChanged(*active);
            }
         }
         return button_one;
      }

      // button 2, debounced; pressed while button 1 is held: a new roll of
      // the current page (the program also plays it: [button:2] audition)
      bool button_two = false;
      elapsedMillis button_two_change;

      bool buttonTwo(bool b1)
      {
         bool now = hw::buttonPressed(1);
         if (now != button_two && button_two_change > BUTTON_DEBOUNCE_MS)
         {
            button_two = now;
            button_two_change = 0;
            if (now && b1 && active && active->canRoll())
            {
               active->roll(active->pagerIndex() >= 0 ? active->page() : -1);
               programSettingsChanged(*active);
               button_one_used = true; // releasing button 1 keeps the page
            }
         }
         return button_two;
      }

      void setStep(Control &t, float position)
      {
         float v = t.fromPosition(constrain(position, 0.0f, 1.0f));
         if (t.step > 0.0f)
            v = t.min + roundf((v - t.min) / t.step) * t.step;
         if (v != t.value)
         {
            t.value = v;
            t.changed = true;
            programSettingsChanged(*active);
         }
      }

      // [edit:] knob: sets the selected setting once it catches its value;
      // with button 1 held it moves the setting on every page by the same step
      void editKnob(Control &c, std::vector<Control> &controls, bool b1)
      {
         float pos = constrain(hw::input(c.input), 0.0f, 1.0f);
         int target = active->editTarget(c);
         if (target != c.editIndex)
         {
            c.editIndex = target;
            c.caught = false;
            c.catchRef = -1.0f;
         }
         if (b1 && active->pagerIndex() >= 0)
         {
            // relative: every page moves by the knob's way since it started
            // (from the values at the start, so stepped settings move too)
            const Control &pager = controls[active->pagerIndex()];
            int first = (int)lroundf(min(pager.min, pager.max));
            int last = min((int)lroundf(max(pager.min, pager.max)), first + Control::MAX_PAGES - 1);
            if (c.relRef < 0.0f)
            {
               if (c.catchRef >= 0.0f && fabsf(pos - c.catchRef) < RELATIVE_STEP)
                  return; // not turned yet
               c.relRef = c.catchRef >= 0.0f ? c.catchRef : pos;
               for (int page = first; page <= last; page++)
               {
                  int t = active->editTarget(c, page);
                  c.relBase[page - first] = t >= 0 ? controls[t].position(controls[t].value) : 0.0f;
               }
            }
            button_one_used = true;
            float way = pos - c.relRef;
            for (int page = first; page <= last; page++)
            {
               int t = active->editTarget(c, page);
               if (t >= 0)
                  setStep(controls[t], c.relBase[page - first] + way);
            }
            c.catchRef = pos;
            return;
         }
         c.relRef = -1.0f;
         if (target < 0)
         {
            c.catchRef = pos;
            return;
         }
         Control &t = controls[target];
         // the target changed elsewhere (a roll, randomize, the list): the
         // knob lets go and catches the new value
         if (c.caught && t.value != c.editSet)
            c.caught = false;
         if (!c.caught && catches(c, pos, t.position(t.value)))
            c.caught = true;
         c.catchRef = pos; // also where a relative edit (button 1) starts from
         if (c.caught)
         {
            setStep(t, pos);
            c.editSet = t.value;
         }
      }
   }

   // ---- watchdog and start counter -----------------------------------------

   // starts that did not reach HEALTHY_MS, in EEPROM (the SNVS general
   // purpose registers didn't keep a value): written at every start and
   // once after HEALTHY_MS; also counts starts that ended by power off
   namespace
   {
      constexpr int BOOT_EEPROM_ADDRESS = 1030;   // magic, count
      constexpr uint8_t BOOT_EEPROM_MAGIC = 0xB7;
      constexpr uint32_t HEALTHY_MS = 10000;      // a start counts as good after this
      bool boot_cleared = false;
      int boot_failures = -1;                     // read once, at the start

      void writeBootCount(int count)
      {
         EEPROM.write(BOOT_EEPROM_ADDRESS, BOOT_EEPROM_MAGIC);
         EEPROM.write(BOOT_EEPROM_ADDRESS + 1, (uint8_t)constrain(count, 0, 255));
      }
   }

   // failed starts before this one
   int bootFailures()
   {
      if (boot_failures < 0)
         boot_failures = EEPROM.read(BOOT_EEPROM_ADDRESS) == BOOT_EEPROM_MAGIC ? EEPROM.read(BOOT_EEPROM_ADDRESS + 1) : 0;
      return boot_failures;
   }

   // RTWDOG (WDOG3) on its 32 kHz clock / 256: TIMEOUT_TICKS = 4 s; the
   // starts so far are counted first (a start that never reaches
   // HEALTHY_MS is a failed one)
   FLASHMEM void watchdogBegin()
   {
      writeBootCount(bootFailures() + 1); // this start, until it proves good
      constexpr uint32_t TIMEOUT_TICKS = 4 * 32768 / 256;
      // every wait bounded: a watchdog that can't be configured must not
      // hang the start it is meant to protect
      __disable_irq();
      WDOG3_CNT = 0xD928C520u; // unlock
      for (int i = 0; i < 100000 && !(WDOG3_CS & (1u << 11)); i++) // ULK
         ;
      WDOG3_WIN = 0;
      WDOG3_TOVAL = TIMEOUT_TICKS;
      // CMD32EN, PRES (/256), CLK = LPO (01), EN, UPDATE
      WDOG3_CS = (1u << 13) | (1u << 12) | (1u << 8) | (1u << 7) | (1u << 5);
      __enable_irq();
      for (int i = 0; i < 1000000 && !(WDOG3_CS & (1u << 10)); i++) // RCS: new settings in use
         ;
      watchdog_cs = WDOG3_CS;
      watchdog_toval = WDOG3_TOVAL;
   }

      void watchdogFeed()
   {
      WDOG3_CNT = 0xB480A602u;
   }

   void update()
   {
      watchdogFeed();
      if (!boot_cleared && millis() > HEALTHY_MS)
      {
         boot_cleared = true; // this start is a good one
         writeBootCount(0);
      }
      if (storePending && sinceSelect > PROGRAM_STORE_DELAY_MS)
      {
         storePending = false;
         EEPROM.write(PROGRAM_EEPROM_ADDRESS, PROGRAM_EEPROM_MAGIC);
         EEPROM.write(PROGRAM_EEPROM_ADDRESS + 1, current);
      }
      settings::service();
      if (active)
      {
         settings::servicePresets(*active); // a preset chosen in the menu
         applyBlockSize(); // the setting may have changed
         if (unmute_at && (int32_t)(millis() - unmute_at) >= 0)
         {
            unmute_at = 0;
            codecWrite(0x07, 0x00); // the DACs after a rate change
         }
         if (wantedSampleRate(nullptr) != tdm::sampleRate())
         {
            // the program's DSP is set up for one rate: start it again
            restarting = true;
            selectProgram(current);
            restarting = false;
         }
         loop::service(*active);
      }
      hw::scan();
      if (active)
      {
         std::vector<Control> &controls = active->controls();
         bool b1 = buttonOne();
         bool b2 = buttonTwo(b1);
         for (Control &c : controls)
         {
            if (c.edit && c.input >= 0)
            {
               editKnob(c, controls, b1);
               continue;
            }
            float v;
            if (c.volts)
            {
               // V/Oct: volts of the bound input and the routed CV inputs, no scaling
               float volts = c.input >= 0 ? hw::inputVolts(c.input) : 0.0f;
               for (uint32_t mask = c.cvMask; mask; mask &= mask - 1)
                  volts += hw::inputVolts(__builtin_ctz(mask));
               *c.zone = volts;
               continue;
            }
            if (c.input >= 0)
            {
               v = hw::input(c.input);
               if (c.knob)
               {
                  // until the knob reaches its stored value, the value stays
                  float stored = c.position(c.value);
                  if (!c.caught && catches(c, v, stored))
                     c.caught = true;
                  if (!c.caught)
                     v = stored;
                  else if (fabsf(v - stored) > KNOB_STORE_STEP)
                  {
                     c.value = c.fromPosition(constrain(v, 0.0f, 1.0f)); // saved with the settings
                     programSettingsChanged(*active);
                  }
               }
            }
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
         if (active->takeMidiEdited())
            programSettingsChanged(*active); // saved after a pause, like an edit
         // after the settings: a roll's values are in place when button 2
         // plays it
         active->setButtons(b1, b2);
         for (const Indicator &i : active->indicators())
            if (i.led >= 0)
               hw::setLed(i.led, *i.zone > 0.5f * (i.min + i.max));
         hw::service(*active);
      }
   }

   volatile bool debugMute = false;
   volatile bool debugSilent = false;

   bool clockOnInput8()
   {
      return systemSettings().clockSource == TransportCore::CV;
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
