// SPDX-License-Identifier: GPL-3.0-or-later
//
// Library for the programmable Eurorack DSP Module NEMESIS
//
// nemesis_mem16.cpp - a program's long memory as 16 bit samples, see
// nemesis_mem16.h
//
// Copyright © 2021 Arev Imer if not mentioned elsewise
// released under GNU GPL v3 or later (see LICENSE)
//

#include "nemesis_mem16.h"
#include <Arduino.h>

namespace nemesis
{
   namespace mem16
   {
      int16_t *buffer[NEMESIS_MEM16_CHANNELS] = {nullptr, nullptr};
      namespace
      {
         int used = 0;
      }

      FLASHMEM bool begin(int n)
      {
         end();
         if (n < 1 || n > NEMESIS_MEM16_CHANNELS)
            return false;
         for (int c = 0; c < n; c++)
         {
            // PSRAM only: 2 MB per channel doesn't fit anywhere else
            buffer[c] = (int16_t *)extmem_calloc(LENGTH, sizeof(int16_t));
            if (!buffer[c] || (uint32_t)buffer[c] < 0x70000000u)
            {
               if (buffer[c])
                  free(buffer[c]); // calloc fell back to RAM: too small
               buffer[c] = nullptr;
               end();
               return false;
            }
            used = c + 1;
         }
         return true;
      }

      FLASHMEM void end()
      {
         for (int c = 0; c < NEMESIS_MEM16_CHANNELS; c++)
         {
            if (buffer[c])
               extmem_free(buffer[c]);
            buffer[c] = nullptr;
         }
         used = 0;
      }

      int channels() { return used; }
   }
}
