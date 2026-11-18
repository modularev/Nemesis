//
// Library for the programmable Eurorack DSP Module NEMESIS
//
// nemesis_remote.cpp - remote control over USB serial, see nemesis_remote.h
//
// Copyright © 2021 Arev Imer if not mentioned elsewise
// released under GNU GPL v3
//

#include "nemesis_remote.h"
#include "nemesis.h"

namespace nemesis
{
   namespace
   {
      // a JSON string (labels, menus and names contain no control characters)
      void str(Print &out, const char *s)
      {
         out.print('"');
         for (; s && *s; s++)
         {
            if (*s == '"' || *s == '\\')
               out.print('\\');
            out.print(*s);
         }
         out.print('"');
      }

      void num(Print &out, float v)
      {
         if (isfinite(v))
            out.print(v, 5);
         else
            out.print('0');
      }

      FLASHMEM void printControls(Print &out, const Program &p)
      {
         out.print("\"controls\":[");
         const std::vector<Control> &controls = p.controls();
         for (size_t i = 0; i < controls.size(); i++)
         {
            const Control &c = controls[i];
            if (i)
               out.print(',');
            out.print("{\"i\":");
            out.print((int)i);
            out.print(",\"label\":");
            str(out, c.label);
            out.print(",\"name\":");
            str(out, c.name ? c.name : c.label);
            out.print(",\"min\":");
            num(out, c.min);
            out.print(",\"max\":");
            num(out, c.max);
            out.print(",\"step\":");
            num(out, c.step);
            // settings: the value set (the menu's); knobs: what they give now
            out.print(",\"value\":");
            num(out, c.input < 0 ? c.value : *c.zone);
            out.print(",\"input\":");
            if (c.input >= 0)
               str(out, inputName(c.input));
            else
               out.print("null");
            if (c.menu)
            {
               out.print(",\"menu\":");
               str(out, c.menu);
            }
            if (c.unit)
            {
               out.print(",\"unit\":");
               str(out, c.unit);
            }
            if (c.section)
            {
               out.print(",\"section\":");
               str(out, c.section);
            }
            out.print(",\"knob\":"); // a knob that takes a set value (rev2 pots), else the input rules
            out.print(c.knob ? "true" : "false");
            out.print(",\"gate\":");
            out.print(c.gate ? "true" : "false");
            out.print(",\"log\":");
            out.print(c.log ? "true" : "false");
            out.print(",\"listed\":");
            out.print(c.listed ? "true" : "false");
            out.print(",\"visible\":");
            out.print(p.visible(c) ? "true" : "false");
            out.print(",\"action\":");
            out.print(c.action);
            out.print(",\"home\":");
            out.print(c.home ? "true" : "false");
            out.print(",\"pager\":");
            out.print(c.pager ? "true" : "false");
            out.print('}');
         }
         out.print(']');
      }

      FLASHMEM void printSystem(Print &out)
      {
         const SystemSettings &s = systemSettings();
         const TransportState &t = transportState();
         out.printf("\"system\":{\"midiChannel\":%d,\"popup\":%d,\"brightness\":%d,\"ledBrightness\":%d,\"cvRangeVolts\":%d,"
                    "\"clockSource\":%d,\"knobJump\":%d,\"tempo\":",
                    s.midiChannel, s.popup ? 1 : 0, s.brightness, s.ledBrightness, s.cvRangeVolts, s.clockSource, s.knobJump ? 1 : 0);
         num(out, s.tempo);
         out.printf(",\"run\":%d,\"clockOffsetMs\":%d,\"clockPpqn\":%d,\"blockSize\":%d,\"sampleRate\":%d},",
                    s.run ? 1 : 0, s.clockOffsetMs, s.clockPpqn, s.blockSize, s.sampleRate);
         out.print("\"transport\":{\"running\":");
         out.print(t.running ? "true" : "false");
         out.print(",\"external\":");
         out.print(t.external ? "true" : "false");
         out.print(",\"bpm\":");
         num(out, t.bpm);
         out.printf("},\"audio\":{\"blockSize\":%d,\"sampleRate\":%d}", blockSize(), sampleRate());
      }
   }

   FLASHMEM void printRemoteState(Print &out)
   {
      out.printf("JSON {\"rev\":%d,\"current\":%d,\"cpu\":", NEMESIS_HW_REV, currentProgram());
      num(out, cpuLoad());
      out.print(",\"programs\":[");
      for (int i = 0; i < programCount(); i++)
      {
         if (i)
            out.print(',');
         str(out, program(i)->name());
      }
      out.print("],");
      Program *p = program(currentProgram());
      if (p)
      {
         printControls(out, *p);
         out.print(',');
      }
      printSystem(out);
      out.println('}');
   }

   FLASHMEM bool setProgramSetting(int index, float value)
   {
      Program *p = program(currentProgram());
      if (!p || index < 0 || index >= (int)p->controls().size())
         return false;
      Control &c = p->controls()[index];
      if (!Program::stored(c))
         return false; // a CV input, edit knob or roll: its value comes from elsewhere
      c.value = constrain(value, min(c.min, c.max), max(c.min, c.max));
      c.changed = true;
      c.caught = !c.knob; // as a preset: a knob takes over when it reaches the value
      c.catchRef = -1.0f;
      programSettingsChanged(*p);
      return true;
   }

   FLASHMEM bool setSystemSetting(const char *name, float value)
   {
      SystemSettings &s = systemSettings();
      int v = (int)lroundf(value);
      if (strcmp(name, "midiChannel") == 0)
         s.midiChannel = constrain(v, 0, 16);
      else if (strcmp(name, "popup") == 0)
         s.popup = v != 0;
      else if (strcmp(name, "knobJump") == 0)
         s.knobJump = v != 0;
      else if (strcmp(name, "brightness") == 0)
         s.brightness = constrain(v, 1, 10);
      else if (strcmp(name, "ledBrightness") == 0)
         s.ledBrightness = constrain(v, 1, 10);
      else if (strcmp(name, "cvRangeVolts") == 0)
         s.cvRangeVolts = v >= 8 ? 8 : 5;
      else if (strcmp(name, "clockSource") == 0)
         s.clockSource = constrain(v, 0, 3);
      else if (strcmp(name, "tempo") == 0)
         s.tempo = constrain(value, 20.0f, 300.0f);
      else if (strcmp(name, "run") == 0)
         s.run = v != 0;
      else if (strcmp(name, "clockOffsetMs") == 0)
         s.clockOffsetMs = constrain(v, -50, 50);
      else if (strcmp(name, "clockPpqn") == 0)
         s.clockPpqn = (v == 1 || v == 2 || v == 4 || v == 24) ? v : 4;
      else if (strcmp(name, "blockSize") == 0)
         s.blockSize = (v == 32 || v == 64 || v == 128) ? v : 0;
      else if (strcmp(name, "sampleRate") == 0)
         s.sampleRate = (v == 48000 || v == 96000) ? v : 0;
      else
         return false;
      systemSettingsChanged();
      return true;
   }
}
