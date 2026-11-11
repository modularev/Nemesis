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
      const char *problem = nullptr; // see sdProblem()
      bool system_pending = false;
      Program *program_pending = nullptr;
      elapsedMillis since_change;

      // fixed-size line reader, lines longer than the buffer are cut
      // a settings file is a few kB: more means a damaged file system (a
      // cluster chain that loops returns data forever and hung the module)
      constexpr uint32_t MAX_FILE_BYTES = 32768;
      uint32_t bytes_read = 0;

      bool readLine(File &file, char *line, size_t size)
      {
         size_t n = 0;
         int c;
         bool any = false;
         while (bytes_read < MAX_FILE_BYTES && (c = file.read()) >= 0)
         {
            bytes_read++;
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

      // settings are written to <path>.NEW and moved over the old file only
      // when complete: a reset while writing leaves the old file intact
      FLASHMEM File openForWrite(const char *path)
      {
         if (!SD.exists(DIRECTORY))
            SD.mkdir(DIRECTORY);
         char temp[56];
         snprintf(temp, sizeof(temp), "%s.NEW", path);
         SD.remove(temp);
         return SD.open(temp, FILE_WRITE);
      }

      // the file, or its .NEW when a reset came between removing the old file
      // and renaming the new one
      FLASHMEM File openForRead(const char *path)
      {
         File file = SD.open(path, FILE_READ);
         if (file)
            return file;
         char temp[56];
         snprintf(temp, sizeof(temp), "%s.NEW", path);
         return SD.open(temp, FILE_READ);
      }

      FLASHMEM void finishWrite(File &file, const char *path)
      {
         file.close();
         char temp[56];
         snprintf(temp, sizeof(temp), "%s.NEW", path);
         SD.remove(path);
         SD.rename(temp, path);
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
         file.printf("clock=%d\n", system_settings.clockSource);
         file.printf("tempo=%g\n", system_settings.tempo);
         file.printf("run=%d\n", system_settings.run ? 1 : 0);
         file.printf("clock_offset=%d\n", system_settings.clockOffsetMs);
         file.printf("clock_ppqn=%d\n", system_settings.clockPpqn);
         finishWrite(file, path);
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
            if (Program::stored(c)) // settings and the knobs' last values
               file.printf("%s=%.6g\n", c.label, c.value);
         for (int i = 0; i < NUM_ROUTABLE_CV; i++)
         {
            int cv = routableCV(i);
            int target = program.route(cv);
            file.printf("%s=%s\n", inputName(cv), target >= 0 ? program.controls()[target].label : "");
         }
         finishWrite(file, path);
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
   const char *sdProblem() { return card && !problem ? nullptr : problem ? problem : "no SD"; }

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
      // a file that reads beyond MAX_FILE_BYTES: renamed to BAD_<name>, not
      // deleted (removing frees its cluster chain, which may loop), and the
      // card is left alone if even that fails
      FLASHMEM void quarantine(File &file, const char *path)
      {
         file.close();
         const char *name = strrchr(path, '/');
         char bad[48];
         snprintf(bad, sizeof(bad), "%s/BAD_%s", DIRECTORY, name ? name + 1 : path);
         Serial.printf("settings: %s is damaged (longer than %lu bytes), ", path, (unsigned long)MAX_FILE_BYTES);
         if (!SD.exists(bad) && SD.rename(path, bad))
            Serial.printf("renamed to %s; repair the card on a computer\n", bad);
         else
         {
            Serial.printf("SD card no longer used this session\n");
            card = false;
         }
         problem = "SD bad";
      }

      FLASHMEM void begin(bool use_card)
      {
         if (!use_card)
         {
            problem = "SD off";
            return;
         }
         card = SD.begin(BUILTIN_SDCARD);
         if (!card)
         {
            problem = "no SD";
            return;
         }
         char path[32];
         snprintf(path, sizeof(path), "%s/SYSTEM.TXT", DIRECTORY);
         File file = openForRead(path);
         if (!file)
            return;
         char line[96], *key, *value;
         bytes_read = 0;
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
            else if (strcmp(key, "clock") == 0)
               system_settings.clockSource = constrain(v, 0, 3);
            else if (strcmp(key, "clock_ppqn") == 0)
               system_settings.clockPpqn = (v == 1 || v == 2 || v == 24) ? v : 4;
            else if (strcmp(key, "tempo") == 0)
               system_settings.tempo = constrain((float)atof(value), 20.0f, 300.0f);
            else if (strcmp(key, "run") == 0)
               system_settings.run = v != 0;
            else if (strcmp(key, "clock_offset") == 0)
               system_settings.clockOffsetMs = constrain(v, -50, 50);
         }
         if (bytes_read >= MAX_FILE_BYTES)
         {
            quarantine(file, path);
            return;
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
         File file = openForRead(path);
         if (!file)
            return;
         char line[96], *key, *value;
         bytes_read = 0;
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
            if (!Program::stored(c))
               continue;
            c.value = constrain((float)atof(value), min(c.min, c.max), max(c.min, c.max));
            c.changed = true;
            c.caught = !c.knob; // the knob takes over when it reaches this value
            c.catchRef = -1.0f;
         }
         if (bytes_read >= MAX_FILE_BYTES)
         {
            quarantine(file, path); // the program keeps its defaults
            return;
         }
         file.close();
      }

      FLASHMEM void flush(Program &program)
      {
         if (program_pending != &program)
            return;
         if (card)
            saveProgram(program);
         program_pending = nullptr;
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

      // ---- presets -----------------------------------------------------
      namespace
      {
         constexpr int MAX_PRESETS = 128;
         constexpr uint32_t MAX_PRESET_BYTES = 131072;
         const Program *preset_program = nullptr;
         int preset_control = -1;     // the [action:preset] setting
         int preset_loaded = -1;      // the line its value stands for
         uint32_t preset_offset[MAX_PRESETS];
         int preset_count = 0;
         char *preset_menu = nullptr; // "'name':0;'name':1;..}" for the setting

         void presetPath(const Program &program, char *path, size_t size)
         {
            snprintf(path, size, "%s/%s.PRE", DIRECTORY, program.name());
         }

         // one line, at most MAX_PRESET_BYTES read in all (see MAX_FILE_BYTES)
         bool readPresetLine(File &file, char *line, size_t size, uint32_t &total)
         {
            size_t n = 0;
            int c;
            bool any = false;
            while (total < MAX_PRESET_BYTES && (c = file.read()) >= 0)
            {
               total++;
               any = true;
               if (c == '\n')
                  break;
               if (c != '\r' && n < size - 1)
                  line[n++] = c;
            }
            line[n] = '\0';
            return any;
         }
      }

      FLASHMEM void loadPresets(Program &program)
      {
         preset_program = &program;
         preset_control = -1;
         preset_count = 0;
         free(preset_menu);
         preset_menu = nullptr;
         std::vector<Control> &controls = program.controls();
         for (int i = 0; i < (int)controls.size(); i++)
            if (controls[i].action == Program::ACTION_PRESET)
               preset_control = i;
         if (preset_control < 0 || !card)
            return;
         preset_loaded = (int)lroundf(controls[preset_control].value); // stored: not applied again

         char path[48];
         presetPath(program, path, sizeof(path));
         File file = SD.open(path, FILE_READ);
         if (!file)
            return;
         // the names, as the menu of the setting
         size_t capacity = 4096, used = 0;
         preset_menu = (char *)malloc(capacity);
         char line[512];
         uint32_t total = 0, offset = 0;
         while (preset_menu && preset_count < MAX_PRESETS && readPresetLine(file, line, sizeof(line), total))
         {
            uint32_t start = offset;
            offset = total;
            if (line[0] == '#' || line[0] == '\0')
               continue;
            char *bar = strchr(line, '|');
            if (bar)
               *bar = '\0';
            for (char *c = line; *c; c++) // characters the menu syntax uses
               if (*c == '\'' || *c == ':' || *c == ';' || *c == '}')
                  *c = ' ';
            char entry[80];
            int n = snprintf(entry, sizeof(entry), "'%.40s':%d;", line, preset_count);
            if (used + n + 2 > capacity)
            {
               capacity *= 2;
               char *bigger = (char *)realloc(preset_menu, capacity);
               if (!bigger)
                  break;
               preset_menu = bigger;
            }
            memcpy(preset_menu + used, entry, n);
            used += n;
            preset_offset[preset_count++] = start;
         }
         file.close();
         if (preset_menu)
         {
            preset_menu[used] = '}';
            preset_menu[used + 1] = '\0';
            controls[preset_control].menu = preset_menu;
         }
         Serial.printf("presets: %d in %s\n", preset_count, path);
      }

      FLASHMEM void servicePresets(Program &program)
      {
         if (&program != preset_program || preset_control < 0 || preset_count == 0)
            return;
         std::vector<Control> &controls = program.controls();
         if (preset_control >= (int)controls.size())
            return;
         int index = constrain((int)lroundf(controls[preset_control].value), 0, preset_count - 1);
         if (index == preset_loaded)
            return;
         preset_loaded = index;
         char path[48];
         presetPath(program, path, sizeof(path));
         File file = SD.open(path, FILE_READ);
         if (!file || !file.seek(preset_offset[index]))
            return;
         char line[1024];
         uint32_t total = 0;
         readPresetLine(file, line, sizeof(line), total);
         file.close();
         // name|label=value|label=value
         int applied = 0;
         char *field = strchr(line, '|');
         while (field)
         {
            *field++ = '\0';
            char *next = strchr(field, '|');
            if (next)
               *next = '\0';
            char *key, *value;
            if (splitLine(field, &key, &value))
            {
               int i = program.controlIndex(key);
               if (i >= 0 && i != preset_control && Program::stored(controls[i]))
               {
                  Control &c = controls[i];
                  c.value = constrain((float)atof(value), min(c.min, c.max), max(c.min, c.max));
                  c.changed = true;
                  c.caught = !c.knob; // a knob takes over when it reaches the preset's value
                  c.catchRef = -1.0f;
                  applied++;
               }
            }
            if (next)
               *next = '|';
            field = next;
         }
         programSettingsChanged(program);
         Serial.printf("preset %d: %s, %d values\n", index, line, applied);
      }

   }
}
