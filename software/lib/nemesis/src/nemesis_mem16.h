//
// Library for the programmable Eurorack DSP Module NEMESIS
//
// nemesis_mem16.h - a program's long memory as 16 bit samples in PSRAM
//
// A program that reads a long memory fast (pollen: grains pitched up at
// 96 kHz) is bound by the PSRAM's bandwidth: every 32 byte cache line holds
// 8 floats but 16 samples of 16 bit, so the same reads need half the line
// fills (96 dB dynamic range is plenty for such a memory). FAUST has no 16
// bit tables, so the program uses these functions through ffunction:
//
//   declare mem16 "2";                       // channels: the library
//                                            // allocates them at start
//   put = ffunction(float nm16_put(int, int, float), "nemesis_mem16.h", "");
//   get = ffunction(float nm16_get(int, float, float), "nemesis_mem16.h", "");
//
//   nm16_put(channel, index, x)   writes x at index (wrapped), returns x
//   nm16_get(channel, pos, after) reads at pos (samples, fractional, wrapped,
//                                 linear interpolation); `after` is ignored:
//                                 pass the value of the put of that sample so
//                                 FAUST computes the put first
//
// Each channel holds NEMESIS_MEM16_LENGTH samples (2^20: 10.9 s at 96 kHz).
// The program writes sample t at index t (from its start) mod the length,
// as FAUST's delay lines do, so the library can save a frozen loop
// (nemesis_loop.h). The functions are inline (no call per sample: a call
// into ITCM goes through a flash veneer).
//
// Copyright © 2021 Arev Imer if not mentioned elsewise
// released under GNU GPL v3
//

#ifndef nemesis_mem16_h
#define nemesis_mem16_h

#include <stdint.h>
#include <math.h>

#define NEMESIS_MEM16_LENGTH (1u << 20)
#define NEMESIS_MEM16_CHANNELS 2

namespace nemesis
{
   namespace mem16
   {
      constexpr uint32_t LENGTH = NEMESIS_MEM16_LENGTH;
      constexpr uint32_t MASK = LENGTH - 1;
      extern int16_t *buffer[NEMESIS_MEM16_CHANNELS];
      // allocates and clears the channels (PSRAM), false if it can't
      bool begin(int channels);
      void end();
      int channels();
   }
}

static inline float nm16_put(int channel, int index, float x)
{
   float s = x * 32767.0f;
   s = s > 32767.0f ? 32767.0f : (s < -32767.0f ? -32767.0f : s);
   nemesis::mem16::buffer[channel][(uint32_t)index & nemesis::mem16::MASK] = (int16_t)lrintf(s);
   return x;
}

static inline float nm16_get(int channel, float pos, float after)
{
   (void)after;
   float fl = floorf(pos);
   int i = (int)fl;
   float f = pos - fl;
   const int16_t *b = nemesis::mem16::buffer[channel];
   float a = b[(uint32_t)i & nemesis::mem16::MASK];
   float c = b[(uint32_t)(i + 1) & nemesis::mem16::MASK];
   return (a + (c - a) * f) * (1.0f / 32767.0f);
}

#endif
