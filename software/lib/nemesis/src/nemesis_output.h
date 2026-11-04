//
// Library for the programmable Eurorack DSP Module NEMESIS
//
// nemesis_output.h
//
// Output stage of the audio outputs: every program gets the setting
// "output" (clip, soft, tape, fold), stored with its other settings. It
// shapes the audio outputs after the FAUST code, before the DAC (1.0 = 5 V):
//
//   clip  nothing: the DAC clips hard above 5 V
//   soft  transparent up to 4.4 V, then bends smoothly into 4.9 V
//   tape  saturates gradually over the whole range (-0.2 dB for small
//         signals, 5 V = 0.78, 3x full scale reaches the ceiling)
//   fold  folds the wave back down at the ceiling (wavefolder: bright, metallic)
//
// A program sets its default and its audio outputs (CV, gate and V/Oct
// outputs must stay exact) with global metadata:
//
//   declare output "clip";            default mode (else soft)
//   declare audio_outputs "1-6";      outputs to shape (else "1 2")
//
// Plain C++: the firmware's architecture file and the simulator use it.
//
// Copyright © 2021 Arev Imer if not mentioned elsewise
// released under GNU GPL v3
//

#ifndef nemesis_output_h
#define nemesis_output_h

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

namespace nemesis
{
   namespace output
   {
      enum Mode { CLIP, SOFT, TAPE, FOLD, NUM_MODES };

      // the setting, as FAUST [style:menu{..}] metadata
      constexpr const char *LABEL = "output";
      constexpr const char *STYLE = "menu{'clip':0;'soft':1;'tape':2;'fold':3}";
      constexpr uint32_t DEFAULT_OUTPUTS = 0x3; // outputs 1 and 2

      // below 1.0, so the output calibration (gain up to ~1.01) can't clip
      constexpr float CEILING = 0.98f;

      // "clip", "soft", ... -> mode, -1 if unknown
      inline int modeFromName(const char *name)
      {
         static const char *const names[] = {"clip", "soft", "tape", "fold"};
         for (int i = 0; i < NUM_MODES; i++)
            if (strcmp(name, names[i]) == 0)
               return i;
         return -1;
      }

      // "1 2", "1-6", "1,3-4" -> bit per output (bit 0 = output 1)
      inline uint32_t parseOutputs(const char *text)
      {
         uint32_t mask = 0;
         const char *p = text;
         while (*p)
         {
            if (*p < '0' || *p > '9')
            {
               p++;
               continue;
            }
            char *end;
            long first = strtol(p, &end, 10), last = first;
            p = end;
            if (*p == '-')
            {
               last = strtol(p + 1, &end, 10);
               p = end;
            }
            for (long o = first; o <= last; o++)
               if (o >= 1 && o <= 32)
                  mask |= 1u << (o - 1);
         }
         return mask;
      }

      inline float soft(float x)
      {
         const float knee = 0.9f * CEILING;
         float a = fabsf(x);
         if (a <= knee)
            return x;
         float t = (a - knee) / (CEILING - knee);
         float y = knee + (CEILING - knee) * t / (1.0f + t);
         return x < 0 ? -y : y;
      }

      // rational tanh approximation, exact slope 1 at 0, reaches 1 at 3
      inline float tape(float x)
      {
         if (x >= 3.0f)
            return CEILING;
         if (x <= -3.0f)
            return -CEILING;
         float x2 = x * x;
         return CEILING * x * (27.0f + x2) / (27.0f + 9.0f * x2);
      }

      // triangle fold: identity inside +-CEILING, mirrored at the ceiling
      inline float fold(float x)
      {
         float u = (x / CEILING + 1.0f) * 0.25f;
         return CEILING * (4.0f * fabsf(u - floorf(u + 0.5f)) - 1.0f);
      }

      inline void shapeBlock(int mode, float *samples, int n)
      {
         switch (mode)
         {
         case SOFT:
            for (int i = 0; i < n; i++)
               samples[i] = soft(samples[i]);
            break;
         case TAPE:
            for (int i = 0; i < n; i++)
               samples[i] = tape(samples[i]);
            break;
         case FOLD:
            for (int i = 0; i < n; i++)
               samples[i] = fold(samples[i]);
            break;
         default:
            break;
         }
      }
   }
}

#endif
