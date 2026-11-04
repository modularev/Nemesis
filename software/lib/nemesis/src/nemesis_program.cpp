//
// Library for the programmable Eurorack DSP Module NEMESIS
//
// nemesis_program.cpp
//
// Binds FAUST parameters to hardware inputs. A parameter is bound by
//   1. metadata for this revision only:  hslider("mix [rev2:POT_D] [rev1:CV_F]", ...)
//   2. metadata for every revision:      hslider("smear [hw:CV_X]", ...)
//   3. an input name as label prefix:    hslider("CV_X: Pitch", ...)
// Parameters without a binding keep their FAUST default value.
// Parameters without a binding become settings, changed in the menu (rev2)
// and stored on the SD card; buttons, the freq/gain/gate parameters of
// polyphonic programs, MIDI clock/start/stop and [hidden:1] are left out.
// [style:menu{'1/8':0;'1/16':1}] names the values on the display, [scale:log]
// maps the knob exponentially (min and max above 0), e.g. for Hz or ms;
// [unit:Hz] is shown after the value.
// Bargraphs: [status:1] shows the value on the home screen (rev2), [led:1]
// lights LED 1 while the value is above half its range.
// [transport:position|increment|running|bpm|external] parameters get the
// transport at the start of every block (see faust/nemesis.lib).
// [volts:1] gives a bound CV input in volts (calibrated on rev2), for V/Oct.
// [mono:note|gate|velocity] parameters follow monophonic MIDI notes.
// Bargraph [limit:1]: a limiter's gain (0..1), shown in dB on the home screen.
// [cv:CV_A] adds a CV input to the parameter (knob + CV, clipped to 0..1),
// ignored where it names the same input (rev1 knobs are summed in hardware).
// This default routing can be changed in the menu.
// Every program also gets the setting "output" (clip/soft/tape/fold) for its
// audio outputs, see nemesis_output.h.
//
// Copyright © 2021 Arev Imer if not mentioned elsewise
// released under GNU GPL v3
//

#include "nemesis.h"

namespace nemesis
{
   namespace
   {
      enum { TRANSPORT_POSITION, TRANSPORT_INCREMENT, TRANSPORT_RUNNING, TRANSPORT_BPM, TRANSPORT_EXTERNAL };
      const char *const transportNames[] = {"position", "increment", "running", "bpm", "external"};
      enum { MONO_NOTE, MONO_GATE, MONO_VELOCITY };
      const char *const monoNames[] = {"note", "gate", "velocity"};

      const char revisionKey[] = {'r', 'e', 'v', '0' + NEMESIS_HW_REV, '\0'};

      // "CV_X: Pitch" -> input of CV_X, displayLabel "Pitch"
      FLASHMEM int inputFromLabel(const char *label, const char **displayLabel)
      {
         const char *colon = strchr(label, ':');
         size_t len = colon ? (size_t)(colon - label) : strlen(label);
         while (len > 0 && label[len - 1] == ' ')
            len--;

         char token[16];
         if (len == 0 || len >= sizeof(token))
            return -1;
         memcpy(token, label, len);
         token[len] = '\0';

         int input = inputIndex(token);
         if (input >= 0 && colon)
         {
            const char *rest = colon + 1;
            while (*rest == ' ')
               rest++;
            if (*rest)
               *displayLabel = rest;
         }
         return input;
      }
   }

   FLASHMEM void Program::declareControl(const char *key, const char *value)
   {
      if (strcmp(key, revisionKey) == 0)
      {
         fPendingInput = value;
         fPendingForRevision = true;
      }
      else if (strcmp(key, "hw") == 0 && !fPendingForRevision)
      {
         fPendingInput = value;
      }
      else if (strcmp(key, "cv") == 0)
      {
         fPendingCV = value;
      }
      else if (strcmp(key, "style") == 0 && strncmp(value, "menu{", 5) == 0)
      {
         fPendingMenu = value + 5;
      }
      else if (strcmp(key, "unit") == 0)
      {
         fPendingUnit = value;
      }
      else if (strcmp(key, "scale") == 0)
      {
         fPendingLog = strcmp(value, "log") == 0 || strcmp(value, "exp") == 0;
      }
      else if (strcmp(key, "transport") == 0)
      {
         for (int i = 0; i < (int)(sizeof(transportNames) / sizeof(transportNames[0])); i++)
            if (strcmp(value, transportNames[i]) == 0)
               fPendingTransport = i;
      }
      else if (strcmp(key, "mono") == 0)
      {
         for (int i = 0; i < 3; i++)
            if (strcmp(value, monoNames[i]) == 0)
               fPendingMono = i;
      }
      else if (strcmp(key, "volts") == 0)
      {
         fPendingVolts = strcmp(value, "0") != 0;
      }
      else if (strcmp(key, "limit") == 0)
      {
         fPendingLimit = strcmp(value, "0") != 0;
      }
      else if (strcmp(key, "status") == 0 && strcmp(value, "0") != 0)
      {
         fPendingStatus = true;
      }
      else if (strcmp(key, "led") == 0)
      {
         fPendingLed = atoi(value) - 1;
      }
      else if ((strcmp(key, "hidden") == 0 && strcmp(value, "0") != 0) ||
               (strcmp(key, "midi") == 0 && (strcmp(value, "clock") == 0 || strcmp(value, "start") == 0 || strcmp(value, "stop") == 0)))
      {
         fPendingHidden = true;
      }
   }

   FLASHMEM void Program::bindControl(const char *label, float *zone, float init, float min, float max, float step, Kind kind)
   {
      if (fPendingTransport >= 0)
      {
         if (zone && kind != BARGRAPH)
            fTransport.push_back({zone, fPendingTransport});
         fPendingHidden = true;
         fPendingInput = nullptr; // never bound to an input either
         fPendingTransport = -1;
      }

      if (fPendingMono >= 0)
      {
         if (zone && kind != BARGRAPH)
            fMono.push_back({zone, fPendingMono});
         fPendingHidden = true;
         fPendingInput = nullptr;
         fPendingMono = -1;
      }

      if (kind == BARGRAPH && zone && (fPendingStatus || fPendingLed >= 0 || fPendingLimit))
         fIndicators.push_back({zone, min, max, label, fPendingMenu, fPendingStatus || fPendingLimit ? -1 : fPendingLed,
                                fPendingUnit, fPendingLimit});

      const char *displayLabel = label;
      int input = fPendingInput ? inputIndex(fPendingInput) : inputFromLabel(label, &displayLabel);
      int cv = fPendingCV ? inputIndex(fPendingCV) : -1;
      if (cv == input)
         cv = -1;
      bool voiceParameter = strcmp(label, "freq") == 0 || strcmp(label, "gain") == 0 || strcmp(label, "gate") == 0;
      bool setting = input < 0 && kind != BUTTON && !voiceParameter && !fPendingHidden;
      if (zone && kind != BARGRAPH && (input >= 0 || setting))
      {
         bool gate = kind != SLIDER;
         bool log = fPendingLog && !gate && min > 0 && max > 0;
         fControls.push_back({zone, min, max, step, input, 0, gate, displayLabel, fPendingMenu, init, false, log, fPendingUnit, fPendingVolts && input >= 0});
         if (cv >= 0 && cv < MAX_INPUTS)
            fRoute[cv] = fControls.size() - 1;
         updateMasks();
      }

      fPendingInput = nullptr;
      fPendingCV = nullptr;
      fPendingMenu = nullptr;
      fPendingLog = false;
      fPendingUnit = nullptr;
      fPendingVolts = false;
      fPendingLimit = false;
      fPendingHidden = false;
      fPendingStatus = false;
      fPendingLed = -1;
      fPendingForRevision = false;
   }

   FLASHMEM void Program::declareProgram(const char *key, const char *value)
   {
      if (strcmp(key, "output") == 0)
      {
         int mode = output::modeFromName(value);
         if (mode >= 0)
            fOutputDefault = mode;
      }
      else if (strcmp(key, "audio_outputs") == 0)
      {
         fAudioOutputs = output::parseOutputs(value);
      }
   }

   FLASHMEM void Program::bindOutputSetting()
   {
      if (!fAudioOutputs || controlIndex(output::LABEL) >= 0)
         return;
      fOutputMode = fOutputDefault; // a kept or stored value follows (restoreBindings, settings)
      declareControl("style", output::STYLE);
      bindControl(output::LABEL, &fOutputMode, fOutputDefault, 0, output::NUM_MODES - 1, 1, SLIDER);
   }

   void Program::beginBlock()
   {
      const TransportState &s = transport::block();
      for (const TransportZone &t : fTransport)
      {
         switch (t.what)
         {
         case TRANSPORT_POSITION:
            *t.zone = s.position;
            break;
         case TRANSPORT_INCREMENT:
            *t.zone = s.increment;
            break;
         case TRANSPORT_RUNNING:
            *t.zone = s.running ? 1 : 0;
            break;
         case TRANSPORT_BPM:
            *t.zone = s.bpm;
            break;
         case TRANSPORT_EXTERNAL:
            *t.zone = s.external ? 1 : 0;
            break;
         }
      }
   }

   void Program::handleMidi(const MidiMessage &m)
   {
      if (fMono.empty() || m.status >= 0xF0)
         return;
      int channel = midiChannel();
      if (channel > 0 && (m.status & 0x0F) != channel - 1)
         return;
      uint8_t type = m.status & 0xF0;
      bool on = type == 0x90 && m.data2 > 0;
      bool off = type == 0x80 || (type == 0x90 && m.data2 == 0);
      if (!on && !off)
         return;

      // note stack: remove the note, put it on top when it starts
      int n = 0;
      for (int i = 0; i < fNoteCount; i++)
         if (fNotes[i] != m.data1)
            fNotes[n++] = fNotes[i];
      fNoteCount = n;
      if (on)
      {
         if (fNoteCount == (int)sizeof(fNotes))
         {
            memmove(fNotes, fNotes + 1, sizeof(fNotes) - 1);
            fNoteCount--;
         }
         fNotes[fNoteCount++] = m.data1;
      }

      for (const TransportZone &z : fMono)
      {
         switch (z.what)
         {
         case MONO_NOTE: // the last held note, or the released one
            *z.zone = fNoteCount > 0 ? fNotes[fNoteCount - 1] : m.data1;
            break;
         case MONO_GATE:
            *z.zone = fNoteCount > 0 ? 1 : 0;
            break;
         case MONO_VELOCITY:
            if (on)
               *z.zone = m.data2 / 127.0f;
            break;
         }
      }
   }

   void Program::countClip(int output, uint32_t samples)
   {
      if (output < 0 || output >= 8 || samples == 0)
         return;
      fClips[output] = fClips[output] + samples;
      fLastClip = millis();
   }

   FLASHMEM void Program::releaseBindings()
   {
      fKeptValues.clear();
      for (const Control &c : fControls)
         if (c.input < 0)
            fKeptValues.push_back({c.label, c.value});
      memcpy(fKeptRoute, fRoute, sizeof(fRoute));
      fKept = true;
      fControls.clear();
      fIndicators.clear();
      fTransport.clear();
      fMono.clear();
      fNoteCount = 0;
      memset(fRoute, -1, sizeof(fRoute));
   }

   FLASHMEM void Program::restoreBindings()
   {
      if (!fKept)
         return;
      fKept = false;
      for (const auto &kept : fKeptValues)
      {
         int index = controlIndex(kept.first);
         if (index >= 0 && fControls[index].input < 0)
         {
            fControls[index].value = kept.second;
            fControls[index].changed = true;
         }
      }
      fKeptValues.clear();
      memcpy(fRoute, fKeptRoute, sizeof(fRoute));
      updateMasks();
   }

   int Program::route(int cvInput) const
   {
      return (cvInput >= 0 && cvInput < MAX_INPUTS) ? fRoute[cvInput] : -1;
   }

   void Program::setRoute(int cvInput, int control)
   {
      if (cvInput < 0 || cvInput >= MAX_INPUTS)
         return;
      fRoute[cvInput] = (control >= 0 && control < (int)fControls.size()) ? control : -1;
      updateMasks();
   }

   FLASHMEM int Program::controlIndex(const char *label) const
   {
      for (size_t i = 0; i < fControls.size(); i++)
         if (strcmp(fControls[i].label, label) == 0)
            return i;
      return -1;
   }

   void Program::updateMasks()
   {
      for (Control &c : fControls)
      {
         uint32_t mask = 0;
         for (int i = 0; i < MAX_INPUTS; i++)
            if (fRoute[i] >= 0 && &fControls[fRoute[i]] == &c && i != c.input)
               mask |= 1u << i;
         if (mask != c.cvMask && c.input < 0)
            c.changed = true; // a setting no longer modulated returns to its value
         c.cvMask = mask;
      }
   }
}
