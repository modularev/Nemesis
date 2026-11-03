//
// Library for the programmable Eurorack DSP Module NEMESIS
//
// nemesis_settings.cpp
//
// Program settings (parameters without a hardware binding, CV routing) and
// system settings, stored as text files on the Teensy's SD card:
//   /NEMESIS/SYSTEM.TXT      key=value per line
//   /NEMESIS/<program>.TXT   <setting label>=<value>, CV_A=<parameter label>
// Changes are written a few seconds after the last one. Without a card the
// settings work but are not kept.
//
// Copyright © 2021 Arev Imer if not mentioned elsewise
// released under GNU GPL v3
//

#include "nemesis.h"
#include "nemesis_settings.h"
#include <SD.h>

namespace nemesis
{
   namespace
   {
      constexpr uint32_t SAVE_DELAY_MS = 2000;
      const char DIRECTORY[] = "/NEMESIS";

      SystemSettings system_settings;
      bool card = false;
      bool system_pending = false;
      Program *program_pending = nullptr;
      elapsedMillis since_change;

      // fixed-size line reader, lines longer than the buffer are cut
      bool readLine(File &file, char *line, size_t size)
      {
         size_t n = 0;
         int c;
         bool any = false;
         while ((c = file.read()) >= 0)
         {
            any = true;
            if (c == '\n')
               break;
            if (c != '\r' && n < size - 1)
               line[n++] = c;
         }
         line[n] = '\0';
         return any;
      }

      // splits "key=value" in place, trims spaces around both
      bool splitLine(char *line, char **key, char **value)
      {
         char *eq = strchr(line, '=');
         if (!eq || line[0] == '#')
            return false;
         *eq = '\0';
         *key = line;
         *value = eq + 1;
         for (char *s : {*key, *value})
         {
            for (char *e = s + strlen(s); e > s && e[-1] == ' '; e--)
               e[-1] = '\0';
         }
         while (**value == ' ')
            (*value)++;
         return true;
      }

      void programPath(const Program &program, char *path, size_t size)
      {
         snprintf(path, size, "%s/%s.TXT", DIRECTORY, program.name());
      }

      FLASHMEM File openForWrite(const char *path)
      {
         if (!SD.exists(DIRECTORY))
            SD.mkdir(DIRECTORY);
         SD.remove(path);
         return SD.open(path, FILE_WRITE);
      }

      FLASHMEM void saveSystem()
      {
         char path[32];
         snprintf(path, sizeof(path), "%s/SYSTEM.TXT", DIRECTORY);
         File file = openForWrite(path);
         if (!file)
            return;
         file.printf("midi_channel=%d\n", system_settings.midiChannel);
         file.printf("popup=%d\n", system_settings.popup ? 1 : 0);
         file.printf("brightness=%d\n", system_settings.brightness);
         file.printf("cv_range=%d\n", system_settings.cvRangeVolts);
         file.close();
      }

      FLASHMEM void saveProgram(const Program &program)
      {
         char path[48];
         programPath(program, path, sizeof(path));
         File file = openForWrite(path);
         if (!file)
            return;
         file.printf("# NEMESIS settings of %s\n", program.name());
         for (const Control &c : program.controls())
            if (c.input < 0)
               file.printf("%s=%.6g\n", c.label, c.value);
         for (int i = 0; i < NUM_ROUTABLE_CV; i++)
         {
            int cv = routableCV(i);
            int target = program.route(cv);
            file.printf("%s=%s\n", inputName(cv), target >= 0 ? program.controls()[target].label : "");
         }
         file.close();
      }
   }

   SystemSettings &systemSettings() { return system_settings; }

   int midiChannel() { return system_settings.midiChannel; }

   void systemSettingsChanged()
   {
      system_pending = true;
      since_change = 0;
   }

   void programSettingsChanged(Program &program)
   {
      if (program_pending && program_pending != &program)
         saveProgram(*program_pending);
      program_pending = &program;
      since_change = 0;
   }

   bool sdCardPresent() { return card; }

   int routableCV(int i)
   {
#if NEMESIS_HW_REV == 2
      return inputIndex("CV_A") + i;
#else
      return -1;
#endif
   }

   namespace settings
   {
      FLASHMEM void begin()
      {
         card = SD.begin(BUILTIN_SDCARD);
         if (!card)
            return;
         char path[32];
         snprintf(path, sizeof(path), "%s/SYSTEM.TXT", DIRECTORY);
         File file = SD.open(path, FILE_READ);
         if (!file)
            return;
         char line[96], *key, *value;
         while (readLine(file, line, sizeof(line)))
         {
            if (!splitLine(line, &key, &value))
               continue;
            int v = atoi(value);
            if (strcmp(key, "midi_channel") == 0)
               system_settings.midiChannel = constrain(v, 0, 16);
            else if (strcmp(key, "popup") == 0)
               system_settings.popup = v != 0;
            else if (strcmp(key, "brightness") == 0)
               system_settings.brightness = constrain(v, 1, 10);
            else if (strcmp(key, "cv_range") == 0)
               system_settings.cvRangeVolts = (v == 5) ? 5 : 8;
         }
         file.close();
      }

      // once per program, after it is started
      FLASHMEM void load(Program &program)
      {
         if (program.settingsLoaded)
            return;
         program.settingsLoaded = true;
         if (!card)
            return;
         char path[48];
         programPath(program, path, sizeof(path));
         File file = SD.open(path, FILE_READ);
         if (!file)
            return;
         char line[96], *key, *value;
         while (readLine(file, line, sizeof(line)))
         {
            if (!splitLine(line, &key, &value))
               continue;
            int cv = inputIndex(key);
            bool routable = false;
            for (int i = 0; i < NUM_ROUTABLE_CV; i++)
               routable |= cv >= 0 && cv == routableCV(i);
            if (routable)
            {
               program.setRoute(cv, *value ? program.controlIndex(value) : -1);
               continue;
            }
            int index = program.controlIndex(key);
            if (index < 0)
               continue;
            Control &c = program.controls()[index];
            if (c.input >= 0)
               continue;
            c.value = constrain((float)atof(value), min(c.min, c.max), max(c.min, c.max));
            c.changed = true;
         }
         file.close();
      }

      void service()
      {
         if (since_change < SAVE_DELAY_MS)
            return;
         if (system_pending)
         {
            system_pending = false;
            if (card)
               saveSystem();
         }
         if (program_pending)
         {
            if (card)
               saveProgram(*program_pending);
            program_pending = nullptr;
         }
      }
   }
}
