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
   }

   FLASHMEM void Program::bindControl(const char *label, float *zone, float min, float max, bool gate)
   {
      // zone is null for outputs (bargraphs), which only reset the pending metadata
      const char *displayLabel = label;
      int input = fPendingInput ? inputIndex(fPendingInput) : inputFromLabel(label, &displayLabel);
      if (zone && input >= 0)
         fControls.push_back({zone, min, max, input, gate, displayLabel});

      fPendingInput = nullptr;
      fPendingForRevision = false;
   }
}
