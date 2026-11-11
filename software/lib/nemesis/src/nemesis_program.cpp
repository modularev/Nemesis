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
// [mono:note|gate|velocity] parameters follow monophonic MIDI notes,
// [mono:note1..4] and [mono:gate1..4] the voices of a paraphonic program,
// [mono:rr1..4] four pitches set in turn, [mono:sp1..4] four pitches spread
// over the notes played together (see nemesis_program.h), [mono:trig] counts
// note-ons (a change per note, also repeated ones). With [chan:N] a mono
// parameter follows MIDI channel base + N - 1 only (base: the MIDI channel
// setting, omni 1), with [note:N] only that note, e.g. a drum voice per
// channel or per note.
// Bargraph [limit:1]: a limiter's gain (0..1), shown in dB on the home screen.
// [cv:CV_A] adds a CV input to the parameter (knob + CV, clipped to 0..1),
// ignored where it names the same input (rev1 knobs are summed in hardware).
// This default routing can be changed in the menu. [setting:0] makes a
// parameter only a CV target, left out of the settings list (with
// [volts:1] a routed CV gives it volts, e.g. V/Oct on any CV A-D).
// [name:lag] shows another name (labels stay unique: settings are stored by
// label), [if:sync] / [ifnot:sync] lists a setting only while the setting
// "sync" is on / off, e.g. a lag in ms or in note values under one name;
// [if:voice=3] while the setting "voice" is 3. [edit:v{voice} pitch] on a
// knob: turning it sets the setting "v3 pitch" (while "voice" is 3), the
// knob popup shows that setting. [pager:1] on a setting: button 1 steps it
// (on release, unless a knob was turned while it was held: then edit knobs
// move that setting on every page by the same amount), the home screen
// shows it with [pagename:v{voice} type] and a strip of [activity:N]
// bargraphs, the settings list the current page's settings first.
// [section:object] starts a header in the settings list. [button:N]: 1
// while button N is held. Knobs (rev2) keep their last value in the
// settings and catch it after a start or program switch: the value moves
// once the knob reaches it.
// Every program also gets the setting "output" (clip/soft/tape/fold) for its
// audio outputs, see nemesis_output.h.
//
// Copyright © 2021 Arev Imer if not mentioned elsewise
// released under GNU GPL v3
//

#include "nemesis.h"
#include "nemesis_hw.h"
#include <algorithm>

namespace nemesis
{
   namespace
   {
      enum { TRANSPORT_POSITION, TRANSPORT_INCREMENT, TRANSPORT_RUNNING, TRANSPORT_BPM, TRANSPORT_EXTERNAL };
      const char *const transportNames[] = {"position", "increment", "running", "bpm", "external"};
      enum { MONO_NOTE, MONO_GATE, MONO_VELOCITY, MONO_TRIG, MONO_SLOT_NOTE = 10, MONO_SLOT_GATE = 20, MONO_RR = 30, MONO_SPREAD = 40 }; // + voice
      const char *const monoNames[] = {"note", "gate", "velocity", "trig"};

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
         for (int i = 0; i < 4; i++)
            if (strcmp(value, monoNames[i]) == 0)
               fPendingMono = i;
         // note1..note4, gate1..gate4, rr1..rr4
         int len = strlen(value);
         if (len == 3 && strncmp(value, "rr", 2) == 0 && value[2] >= '1' && value[2] < '1' + MAX_SLOTS)
            fPendingMono = MONO_RR + value[2] - '1';
         if (len == 3 && strncmp(value, "sp", 2) == 0 && value[2] >= '1' && value[2] < '1' + MAX_SLOTS)
            fPendingMono = MONO_SPREAD + value[2] - '1';
         if (len == 5 && value[4] >= '1' && value[4] < '1' + MAX_SLOTS)
         {
            int slot = value[4] - '1';
            if (strncmp(value, "note", 4) == 0)
               fPendingMono = MONO_SLOT_NOTE + slot;
            else if (strncmp(value, "gate", 4) == 0)
               fPendingMono = MONO_SLOT_GATE + slot;
            if (slot + 1 > fSlots)
               fSlots = slot + 1;
         }
      }
      else if (strcmp(key, "chan") == 0)
      {
         fPendingChan = constrain(atoi(value), 1, 16);
      }
      else if (strcmp(key, "note") == 0)
      {
         fPendingKey = constrain(atoi(value), 0, 127);
      }
      else if (strcmp(key, "volts") == 0)
      {
         fPendingVolts = strcmp(value, "0") != 0;
      }
      else if (strcmp(key, "loop") == 0)
      {
         fPendingLoop = value;
      }
      else if (strcmp(key, "setting") == 0)
      {
         fPendingUnlisted = strcmp(value, "0") == 0;
      }
      else if (strcmp(key, "name") == 0)
      {
         fPendingName = value;
      }
      else if (strcmp(key, "edit") == 0)
      {
         fPendingEdit = value;
      }
      else if (strcmp(key, "title") == 0)
      {
         fPendingTitle = value;
      }
      else if (strcmp(key, "section") == 0)
      {
         fPendingSection = value;
      }
      else if (strcmp(key, "pager") == 0)
      {
         fPendingPager = strcmp(value, "0") != 0;
      }
      else if (strcmp(key, "action") == 0)
      {
         fPendingAction = strcmp(value, "randomize") == 0 ? ACTION_RANDOMIZE
                          : strcmp(value, "amount") == 0  ? ACTION_AMOUNT
                          : strcmp(value, "roll") == 0    ? ACTION_ROLL
                          : strcmp(value, "target") == 0  ? ACTION_TARGET
                          : strcmp(value, "preset") == 0  ? ACTION_PRESET
                                                          : 0;
      }
      else if (strcmp(key, "random") == 0)
      {
         fPendingRandom = strcmp(value, "0") != 0;
      }
      else if (strcmp(key, "pagename") == 0)
      {
         fPendingPageName = value;
      }
      else if (strcmp(key, "button") == 0)
      {
         fPendingButton = constrain(atoi(value), 1, 2) - 1;
      }
      else if (strcmp(key, "activity") == 0)
      {
         fPendingActivity = atoi(value);
      }
      else if (strcmp(key, "if") == 0 || strcmp(key, "ifnot") == 0)
      {
         fPendingCond = value;
         fPendingCondOn = strcmp(key, "if") == 0;
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
      else if (strcmp(key, "hidden") == 0 && strcmp(value, "0") != 0)
      {
         fPendingHidden = true;
      }
      else if (strcmp(key, "midi") == 0)
      {
         fPendingMidi = value;
         // set by the transport, never a setting
         if (strcmp(value, "clock") == 0 || strcmp(value, "start") == 0 || strcmp(value, "stop") == 0)
            fPendingHidden = true;
      }
   }

   FLASHMEM void Program::bindControl(const char *label, float *zone, float init, float min, float max, float step, Kind kind)
   {
      if (fPendingLoop && zone)
      {
         if (strcmp(fPendingLoop, "length") == 0 && kind == BARGRAPH)
            fLoopLength = zone;
         else if (strcmp(fPendingLoop, "freeze") == 0 && kind != BARGRAPH)
            fLoopFreeze = zone; // still a normal setting
         else if (strcmp(fPendingLoop, "restored") == 0 && kind != BARGRAPH)
         {
            fLoopRestored = zone;
            fPendingHidden = true;
            fPendingInput = nullptr;
         }
      }
      fPendingLoop = nullptr;
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
         {
            TransportZone z = {zone, fPendingMono};
            z.chan = fPendingChan;
            z.key = fPendingKey;
            fMono.push_back(z);
         }
         fPendingHidden = true;
         fPendingInput = nullptr;
         fPendingMono = -1;
      }
      fPendingChan = 0;
      fPendingKey = -1;

      if (fPendingButton >= 0)
      {
         if (zone && kind != BARGRAPH)
            fButtons.push_back({zone, fPendingButton});
         fPendingHidden = true;
         fPendingInput = nullptr;
      }

      if (kind == BARGRAPH && zone && (fPendingStatus || fPendingLed >= 0 || fPendingLimit || fPendingActivity >= 0))
      {
         fIndicators.push_back({zone, min, max, label, fPendingMenu, fPendingStatus || fPendingLimit ? -1 : fPendingLed,
                                fPendingUnit, fPendingLimit});
         fIndicators.back().activity = fPendingActivity;
      }

      const char *displayLabel = label;
      int input = fPendingInput ? inputIndex(fPendingInput) : inputFromLabel(label, &displayLabel);
      int cv = fPendingCV ? inputIndex(fPendingCV) : -1;
      if (cv == input || NUM_ROUTABLE_CV == 0) // rev1 sums knob and CV in hardware
         cv = -1;
      bool voiceParameter = strcmp(label, "freq") == 0 || strcmp(label, "gain") == 0 || strcmp(label, "gate") == 0;
      bool setting = input < 0 && kind != BUTTON && !voiceParameter && !fPendingHidden;
      if (zone && kind != BARGRAPH && (input >= 0 || setting))
      {
         bool gate = kind != SLIDER;
         bool log = fPendingLog && !gate && min > 0 && max > 0;
         fControls.push_back({zone, min, max, step, input, 0, gate, displayLabel, fPendingMenu, init, false, log, fPendingUnit,
                              fPendingVolts, !fPendingUnlisted});
         fControls.back().name = fPendingName;
         Control &added = fControls.back();
         added.cond = fPendingCond;
         added.condOn = fPendingCondOn;
         if (fPendingCond)
         {
            const char *eq = strchr(fPendingCond, '=');
            added.condLen = eq ? eq - fPendingCond : strlen(fPendingCond);
            added.condEquals = eq != nullptr;
            added.condValue = eq ? atof(eq + 1) : 0.0f;
         }
         if (input >= 0)
            added.edit = fPendingEdit;
         added.knob = input >= 0 && hw::isKnob(input) && !added.volts;
         added.section = fPendingSection;
         added.title = fPendingTitle;
         added.action = fPendingAction;
         added.randomizable = fPendingRandom && !fPendingAction;
         added.pageName = fPendingPageName;
         if (fPendingPager && input < 0)
         {
            added.pager = true;
            added.listed = false; // button 1 steps it, the titles show it
            fPager = fControls.size() - 1;
         }
         if (cv >= 0 && cv < MAX_INPUTS)
            fRoute[cv] = fControls.size() - 1;
         updateMasks();
      }
      if (fPendingMidi && zone && kind != BARGRAPH)
         addMidiZone(fPendingMidi, zone, min, max);
      fPendingMidi = nullptr;

      fPendingInput = nullptr;
      fPendingCV = nullptr;
      fPendingMenu = nullptr;
      fPendingLog = false;
      fPendingUnit = nullptr;
      fPendingVolts = false;
      fPendingUnlisted = false;
      fPendingName = nullptr;
      fPendingCond = nullptr;
      fPendingCondOn = true;
      fPendingEdit = nullptr;
      fPendingSection = nullptr;
      fPendingTitle = nullptr;
      fPendingAction = 0;
      fPendingRandom = true;
      fPendingPageName = nullptr;
      fPendingPager = false;
      fPendingButton = -1;
      fPendingActivity = -1;
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
      else if (strcmp(key, "block_size") == 0)
      {
         fBlockSize = atoi(value);
      }
      else if (strcmp(key, "sample_rate") == 0)
      {
         fSampleRate = atoi(value);
      }
      else if (strcmp(key, "cpu_mhz") == 0)
      {
         fCpuMHz = atoi(value);
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

   // [mono:..] parameters with [chan:N] or [note:N]: each follows only its
   // channel (base + N - 1, the base the MIDI channel setting, omni: 1)
   // and / or note, e.g. one drum voice per channel
   void Program::handleFilteredMidi(const MidiMessage &m, bool on, bool off)
   {
      int base = midiChannel() > 0 ? midiChannel() : 1;
      int channel = (m.status & 0x0F) + 1;
      for (TransportZone &z : fMono)
      {
         if (z.chan == 0 && z.key < 0)
            continue;
         if (z.chan > 0 && channel != (base - 1 + z.chan - 1) % 16 + 1)
            continue;
         if (z.chan == 0 && midiChannel() > 0 && channel != midiChannel())
            continue;
         if (z.key >= 0 && m.data1 != z.key)
            continue;
         if (on)
            z.held++;
         else if (z.held > 0)
            z.held--;
         switch (z.what)
         {
         case MONO_NOTE:
            if (on)
               *z.zone = m.data1;
            break;
         case MONO_GATE:
            *z.zone = z.held > 0 ? 1 : 0;
            break;
         case MONO_VELOCITY:
            if (on)
               *z.zone = m.data2 / 127.0f;
            break;
         case MONO_TRIG: // counts the note-ons: every one is a change
            if (on)
               *z.zone = *z.zone + 1.0f >= 1000000.0f ? 1.0f : *z.zone + 1.0f;
            break;
         }
      }
   }

   namespace
   {
      enum { MIDI_CTRL, MIDI_KEYON, MIDI_KEYOFF, MIDI_KEY, MIDI_KEYPRESS, MIDI_PGM, MIDI_CHANPRESS, MIDI_PITCHWHEEL,
             MIDI_START, MIDI_STOP, MIDI_CLOCK };
   }

   // FAUST's MidiUI::addGenericZone: "ctrl N", "ctrl N C", ...
   FLASHMEM void Program::addMidiZone(const char *meta, float *zone, float min, float max)
   {
      static const struct
      {
         const char *name;
         uint8_t type;
         bool number; // followed by a controller or note number
      } KINDS[] = {{"ctrl", MIDI_CTRL, true},          {"keyon", MIDI_KEYON, true},         {"keyoff", MIDI_KEYOFF, true},
                   {"keypress", MIDI_KEYPRESS, true},  {"key", MIDI_KEY, true},             {"pgm", MIDI_PGM, false},
                   {"chanpress", MIDI_CHANPRESS, false}, {"pitchwheel", MIDI_PITCHWHEEL, false}, {"pitchbend", MIDI_PITCHWHEEL, false},
                   {"start", MIDI_START, false},       {"stop", MIDI_STOP, false},          {"clock", MIDI_CLOCK, false}};
      for (const auto &k : KINDS)
      {
         size_t len = strlen(k.name);
         if (strncmp(meta, k.name, len) != 0 || (meta[len] != 0 && meta[len] != ' '))
            continue;
         const char *p = meta + len;
         MidiZone z = {zone, min, max, -1, k.type, 0, 0, false};
         char *end;
         if (k.number)
         {
            long n = strtol(p, &end, 10);
            if (end == p || n < 0 || n > 127)
               return;
            z.num = n;
            p = end;
         }
         long c = strtol(p, &end, 10);
         if (end != p)
            z.chan = constrain(c, 0, 16);
         // the control made from this parameter, if any (just added)
         if (!fControls.empty() && fControls.back().zone == zone)
         {
            z.control = fControls.size() - 1;
            z.log = fControls.back().log;
         }
         fMidiZones.push_back(z);
         return;
      }
      Serial.printf("%s: [midi:%s] not supported\n", name(), meta);
   }

   // in the audio interrupt
   void Program::setFromMidi(const MidiZone &z, float value)
   {
      if (z.control >= 0 && z.control < (int)fControls.size())
      {
         Control &c = fControls[z.control];
         if (c.input < 0 || c.knob)
         {
            // a setting or knob takes it as its value (menu, saved); a knob
            // catches it when turned
            c.value = value;
            c.changed = true;
            if (c.knob)
               c.caught = false;
            fMidiEdited = true;
            return;
         }
      }
      *z.zone = value;
   }

   void Program::handleMidiZones(const MidiMessage &m)
   {
      uint8_t type = m.status & 0xF0;
      int channel = (m.status & 0x0F) + 1;
      for (const MidiZone &z : fMidiZones)
      {
         if (m.status >= 0xF0)
         {
            if (z.type == MIDI_START && (m.status == 0xFA || m.status == 0xFB))
               *z.zone = 1.0f;
            else if (z.type == MIDI_STOP && m.status == 0xFC)
               *z.zone = 0.0f;
            else if (z.type == MIDI_CLOCK && m.status == 0xF8)
               *z.zone = *z.zone > 0.5f ? 0.0f : 1.0f; // FAUST: toggles on every clock
            continue;
         }
         if (z.chan && z.chan != channel)
            continue;
         // FAUST's converters: 0..127 linear (log with [scale:log]) to the range
         auto scaled = [&](int v) {
            float x = v / 127.0f;
            return z.log ? z.min * powf(z.max / z.min, x) : z.min + x * (z.max - z.min);
         };
         switch (z.type)
         {
         case MIDI_CTRL:
            if (type == 0xB0 && m.data1 == z.num)
               setFromMidi(z, scaled(m.data2));
            break;
         case MIDI_KEYON:
            if (type == 0x90 && m.data1 == z.num)
               setFromMidi(z, scaled(m.data2));
            break;
         case MIDI_KEYOFF:
            if ((type == 0x80 || (type == 0x90 && m.data2 == 0)) && m.data1 == z.num)
               setFromMidi(z, scaled(m.data2));
            break;
         case MIDI_KEY: // velocity on, 0 off
            if ((type == 0x90 || type == 0x80) && m.data1 == z.num)
               setFromMidi(z, scaled(type == 0x90 ? m.data2 : 0));
            break;
         case MIDI_KEYPRESS:
            if (type == 0xA0 && m.data1 == z.num)
               setFromMidi(z, scaled(m.data2));
            break;
         case MIDI_PGM: // the program number, if in the range
            if (type == 0xC0 && m.data1 >= z.min && m.data1 <= z.max)
               setFromMidi(z, m.data1);
            break;
         case MIDI_CHANPRESS:
            if (type == 0xD0)
               setFromMidi(z, scaled(m.data1));
            break;
         case MIDI_PITCHWHEEL:
            if (type == 0xE0)
            {
               // FAUST: 0..8191..16383 to min..0..max (or the middle when 0
               // isn't in the range)
               int wheel = m.data1 | m.data2 << 7;
               float mid = (z.min <= 0.0f && z.max >= 0.0f) ? 0.0f : 0.5f * (z.min + z.max);
               float v = wheel <= 8191 ? z.min + (mid - z.min) * wheel / 8191.0f
                                       : mid + (z.max - mid) * (wheel - 8191) / 8192.0f;
               setFromMidi(z, v);
            }
            break;
         }
      }
   }

   void Program::handleMidi(const MidiMessage &m)
   {
      if (!fMidiZones.empty())
      {
         int listen = midiChannel();
         if (m.status >= 0xF0 || listen == 0 || (m.status & 0x0F) == listen - 1)
            handleMidiZones(m);
      }
      if (fMono.empty() || m.status >= 0xF0)
         return;
      uint8_t type = m.status & 0xF0;
      bool on = type == 0x90 && m.data2 > 0;
      bool off = type == 0x80 || (type == 0x90 && m.data2 == 0);
      if (!on && !off)
         return;
      handleFilteredMidi(m, on, off);
      int channel = midiChannel();
      if (channel > 0 && (m.status & 0x0F) != channel - 1)
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

      // round robin: a note alone sets all four, added notes go in turn
      if (on)
      {
         if (fNoteCount == 1)
         {
            memset(fRR, m.data1, sizeof(fRR));
            fRRNext = 1;
         }
         else
         {
            fRR[fRRNext] = m.data1;
            fRRNext = (fRRNext + 1) % MAX_SLOTS;
         }
      }

      // spread: the notes played together share the four pitches
      if (on)
      {
         if (fNoteCount == 1)
            fChordCount = 0; // played alone: start anew
         if (fChordCount == MAX_SLOTS)
         {
            memmove(fChord, fChord + 1, MAX_SLOTS - 1); // the oldest goes
            fChordCount--;
         }
         fChord[fChordCount++] = m.data1;
         uint8_t sorted[MAX_SLOTS];
         memcpy(sorted, fChord, fChordCount);
         std::sort(sorted, sorted + fChordCount);
         // which note each pitch plays, by the number of notes
         static const uint8_t SPREAD[MAX_SLOTS][MAX_SLOTS] = {{0, 0, 0, 0}, {0, 0, 1, 1}, {0, 0, 1, 2}, {0, 1, 2, 3}};
         for (int i = 0; i < MAX_SLOTS; i++)
            fSpread[i] = sorted[SPREAD[fChordCount - 1][i]];
      }

      // paraphonic voices
      if (fSlots > 0)
      {
         int slot = -1;
         for (int i = 0; i < fSlots; i++) // the voice holding this note
            if (fSlotNote[i] == m.data1 && fSlotOn[i])
               slot = i;
         if (on && slot < 0)
         {
            // the lowest free voice (keys fill voices 1, 2, 3 .. in order),
            // else the oldest note is stolen
            for (int i = 0; i < fSlots && slot < 0; i++)
               if (!fSlotOn[i])
                  slot = i;
            if (slot < 0)
            {
               slot = 0;
               for (int i = 1; i < fSlots; i++)
                  if (fSlotAge[i] < fSlotAge[slot])
                     slot = i;
            }
         }
         if (slot >= 0)
         {
            if (on)
               fSlotNote[slot] = m.data1;
            fSlotOn[slot] = on;
            fSlotAge[slot] = ++fSlotClock;
         }
      }

      for (const TransportZone &z : fMono)
      {
         if (z.chan > 0 || z.key >= 0)
            continue; // handleFilteredMidi()
         switch (z.what)
         {
         case MONO_TRIG:
            if (on)
               *z.zone = *z.zone + 1.0f >= 1000000.0f ? 1.0f : *z.zone + 1.0f;
            break;
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
         default:
            if (z.what >= MONO_SPREAD)
            {
               if (fSpread[z.what - MONO_SPREAD] < 128)
                  *z.zone = fSpread[z.what - MONO_SPREAD];
            }
            else if (z.what >= MONO_RR)
            {
               if (fRR[z.what - MONO_RR] < 128)
                  *z.zone = fRR[z.what - MONO_RR];
            }
            else if (z.what >= MONO_SLOT_GATE)
               *z.zone = fSlotOn[z.what - MONO_SLOT_GATE] ? 1 : 0;
            else if (z.what >= MONO_SLOT_NOTE && fSlotNote[z.what - MONO_SLOT_NOTE] < 128)
               *z.zone = fSlotNote[z.what - MONO_SLOT_NOTE];
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
         if (stored(c))
            fKeptValues.push_back({c.label, c.value});
      memcpy(fKeptRoute, fRoute, sizeof(fRoute));
      fKept = true;
      fKeptValues.shrink_to_fit();
      // clear() keeps a vector's memory: swap with an empty one to free it
      // (strike's ~200 controls kept ~40 kB of the heap after it stopped)
      std::vector<Control>().swap(fControls);
      std::vector<Indicator>().swap(fIndicators);
      std::vector<std::vector<float>>().swap(fRolls); // the history points into this program's controls
      fRollPos = 0;
      fLoopLength = fLoopFreeze = fLoopRestored = nullptr;
      std::vector<std::pair<float *, int>>().swap(fButtons);
      fPager = -1;
      std::vector<TransportZone>().swap(fTransport);
      std::vector<TransportZone>().swap(fMono);
      std::vector<MidiZone>().swap(fMidiZones);
      fNoteCount = 0;
      fSlots = 0;
      memset(fSlotOn, 0, sizeof(fSlotOn));
      memset(fSlotNote, 255, sizeof(fSlotNote));
      memset(fRR, 255, sizeof(fRR));
      fRRNext = 1;
      memset(fSpread, 255, sizeof(fSpread));
      fChordCount = 0;
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
         if (index >= 0 && stored(fControls[index]))
         {
            Control &c = fControls[index];
            c.value = kept.second;
            c.changed = true;
            c.caught = !c.knob; // a knob catches its last value
            c.catchRef = -1.0f;
         }
      }
      std::vector<std::pair<const char *, float>>().swap(fKeptValues);
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

   bool Program::isCondition(const Control &setting, const Control &c) const
   {
      return c.cond && strncmp(c.cond, setting.label, c.condLen) == 0 && setting.label[c.condLen] == '\0';
   }

   bool Program::visible(const Control &c) const
   {
      if (!c.cond)
         return true;
      for (const Control &other : fControls)
      {
         if (!isCondition(other, c))
            continue;
         float v = other.input < 0 ? other.value : *other.zone;
         bool holds = c.condEquals ? fabsf(v - c.condValue) < 0.5f : v > 0.5f;
         return holds == c.condOn;
      }
      return true;
   }

   int Program::page() const
   {
      if (fPager < 0)
         return 0;
      const Control &p = fControls[fPager];
      return (int)lroundf(p.value);
   }

   void Program::nextPage()
   {
      if (fPager < 0)
         return;
      Control &p = fControls[fPager];
      float lo = min(p.min, p.max), hi = max(p.min, p.max);
      float step = p.step > 0.0f ? p.step : 1.0f;
      p.value = p.value + step > hi + 0.001f ? lo : p.value + step;
      p.changed = true;
   }

   void Program::randomize(int page)
   {
      static uint32_t seed = 0x9E3779B9u ^ micros();
      float amount = 1.0f;
      for (const Control &c : fControls)
         if (c.action == ACTION_AMOUNT)
            amount = constrain(c.position(c.value), 0.0f, 1.0f);
      for (Control &c : fControls)
      {
         if (c.input >= 0 || !c.listed || !c.randomizable || c.pager || c.volts)
            continue;
         if (fPager >= 0)
         {
            // only settings that belong to a page: that page, or all
            if (!c.condEquals || !isCondition(fControls[fPager], c))
               continue;
            if (page >= 0 && (int)lroundf(c.condValue) != page)
               continue;
         }
         // the pots and patched CVs play these: they stay
         if (held(c, fPager >= 0 ? (int)lroundf(c.condValue) : -1))
            continue;
         seed ^= seed << 13;
         seed ^= seed >> 17;
         seed ^= seed << 5;
         float r = (seed >> 8) / 16777216.0f; // 0..1
         float pos = c.position(c.value);
         pos += amount * (r - pos);
         float v = c.fromPosition(constrain(pos, 0.0f, 1.0f));
         if (c.step > 0.0f)
            v = c.min + roundf((v - c.min) / c.step) * c.step;
         c.value = constrain(v, min(c.min, c.max), max(c.min, c.max));
         c.changed = true;
      }
   }

   bool Program::rollable(const Control &c) const
   {
      if (c.input >= 0 || !c.listed || !c.randomizable || c.pager || c.volts)
         return false;
      return fPager < 0 || (c.condEquals && isCondition(fControls[fPager], c));
   }

   void Program::snapshot(std::vector<float> &values) const
   {
      values.clear();
      for (const Control &c : fControls)
         if (rollable(c))
            values.push_back(c.value);
   }

   void Program::restore(const std::vector<float> &values)
   {
      size_t i = 0;
      for (Control &c : fControls)
         if (rollable(c) && i < values.size())
         {
            c.value = values[i++];
            c.changed = true;
         }
   }

   bool Program::held(const Control &c, int page) const
   {
      // CV A-D routed to it with a signal (an unpatched jack reads 0 V)
      for (uint32_t mask = c.cvMask; mask; mask &= mask - 1)
         if (fabsf(hw::input(__builtin_ctz(mask))) > 0.02f)
            return true;
      // an edit knob sets it on this page
      int index = &c - fControls.data();
      for (const Control &knob : fControls)
         if (knob.edit && editTarget(knob, page) == index)
            return true;
      return false;
   }

   bool Program::canRoll() const
   {
      for (const Control &c : fControls)
         if (c.action == ACTION_ROLL || c.action == ACTION_RANDOMIZE)
            return true;
      return false;
   }

   int Program::rollTarget() const
   {
      for (const Control &c : fControls)
         if (c.action == ACTION_TARGET)
            return lroundf(c.value) == 0 ? page() : -1;
      return fPager >= 0 ? page() : -1;
   }

   void Program::showRoll()
   {
      fRollMillis = millis();
      for (Control &c : fControls)
         if (c.action == ACTION_ROLL)
         {
            c.value = fRollPos;
            c.changed = true;
         }
   }

   void Program::roll(int page)
   {
      if (fRolls.empty())
      {
         fRolls.emplace_back();
         snapshot(fRolls.back()); // #0: before rolling
         fRollPos = 0;
      }
      fRolls.resize(fRollPos + 1); // a new roll after going back replaces the later ones
      randomize(page);
      fRolls.emplace_back();
      snapshot(fRolls.back());
      if ((int)fRolls.size() > MAX_ROLLS + 1)
         fRolls.erase(fRolls.begin() + 1); // the oldest roll goes, #0 stays
      fRollPos = fRolls.size() - 1;
      showRoll();
   }

   void Program::rollBack()
   {
      if (fRollPos <= 0 || fRolls.empty())
         return;
      fRollPos--;
      restore(fRolls[fRollPos]);
      showRoll();
   }

   void Program::actionDone(Control &c)
   {
      if (c.action != ACTION_RANDOMIZE)
         return;
      int choice = (int)lroundf(c.value);
      if (choice > 0)
         randomize(choice == 1 ? page() : -1);
      c.value = 0; // back to "off": the next confirm rolls again
      c.changed = true;
   }

   void Program::setButtons(bool b1, bool b2)
   {
      for (auto &b : fButtons)
         *b.first = (b.second == 0 ? b1 : b2) ? 1.0f : 0.0f;
   }

   int Program::editTarget(const Control &knob, int page) const
   {
      return knob.edit ? templateTarget(knob.edit, page) : -1;
   }

   int Program::templateTarget(const char *tmpl, int page) const
   {
      if (!tmpl)
         return -1;
      // "v{voice} pitch" -> "v3 pitch"
      char label[48];
      const char *open = strchr(tmpl, '{');
      const char *close = open ? strchr(open, '}') : nullptr;
      if (!open || !close)
         snprintf(label, sizeof(label), "%s", tmpl);
      else
      {
         char name[24];
         snprintf(name, sizeof(name), "%.*s", (int)(close - open - 1), open + 1);
         int index = controlIndex(name);
         if (index < 0)
            return -1;
         const Control &sel = fControls[index];
         int value = page >= 0 ? page : (int)lroundf(sel.input < 0 ? sel.value : *sel.zone);
         snprintf(label, sizeof(label), "%.*s%d%s", (int)(open - tmpl), tmpl, value, close + 1);
      }
      int target = controlIndex(label);
      return (target >= 0 && fControls[target].input < 0) ? target : -1;
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
