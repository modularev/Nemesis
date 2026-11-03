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
// [style:menu{'1/8':0;'1/16':1}] names the values on the display.
// Bargraphs: [status:1] shows the value on the home screen (rev2), [led:1]
// lights LED 1 while the value is above half its range.
// [cv:CV_A] adds a CV input to the parameter (knob + CV, clipped to 0..1),
// ignored where it names the same input (rev1 knobs are summed in hardware).
// This default routing can be changed in the menu.
//
// Copyright © 2021 Arev Imer if not mentioned elsewise
// released under GNU GPL v3
//

#include "nemesis.h"

namespace nemesis
{
   namespace
   {
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
      if (kind == BARGRAPH && zone && (fPendingStatus || fPendingLed >= 0))
         fIndicators.push_back({zone, min, max, label, fPendingMenu, fPendingStatus ? -1 : fPendingLed});

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
         fControls.push_back({zone, min, max, step, input, 0, gate, displayLabel, fPendingMenu, init, false});
         if (cv >= 0 && cv < MAX_INPUTS)
            fRoute[cv] = fControls.size() - 1;
         updateMasks();
      }

      fPendingInput = nullptr;
      fPendingCV = nullptr;
      fPendingMenu = nullptr;
      fPendingHidden = false;
      fPendingStatus = false;
      fPendingLed = -1;
      fPendingForRevision = false;
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
