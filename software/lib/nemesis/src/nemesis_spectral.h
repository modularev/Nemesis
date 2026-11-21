// SPDX-License-Identifier: GPL-3.0-or-later
//
// Library for the programmable Eurorack DSP Module NEMESIS
//
// nemesis_spectral.h - spectral processing (a phase vocoder) for FAUST programs
//
// FAUST computes sample by sample and can't express "once per block", so a
// program reaches the spectrum through this function (ffunction):
//
//   declare spectral "2";                      // channels: the library
//                                              // allocates them at start
//   spectral = ffunction(float nm_spectral(int, int, int, float, float, float, float, float, float, float, float,
//                        float, float, float, float), "nemesis_spectral.h", "");
//   y = spectral(channel, kernel, size, x, a, b, c, d, e, f, g, h, i, j, k);
//
// Per sample nm_spectral() puts x into a ring and returns the next sample
// of the overlap-added output; every HOP = size / 4 samples it runs a frame:
// the last `size` input samples, Hann windowed, through the FFT, a kernel
// on the bins (the parameters a..k as they were on that sample), the
// inverse FFT, the window again, added into the output ring. The synthesis
// (kernel, inverse FFT, overlap-add) runs hop / 4 samples after the
// analysis, so the halves of a frame fall into different blocks: the output
// is size + size / 16 samples late. Channel 1 runs half a hop after channel 0, so their
// frames fall into different audio blocks. CMSIS-DSP's real FFT on the
// Teensy, a plain FFT elsewhere (tests on the Mac).
//
// size: 1024 or 2048 points. A program can change it while running (a
// setting): the library reallocates in the main loop (service()), the
// output is silent for that moment.
//
// Kernels:
//   FREEZE (0): the output is the wet sound only (the program mixes it
//     with the dry signal, which never goes through the FFT: no comb).
//     Two sources, crossfading:
//     - smear (d > 0): every bin's level and phase turn follow the input
//       slowly (d seconds, tilt g), a phase vocoder with inertia: a
//       continuously smeared, morphing copy of the input
//     - freeze: the levels of the last 32 frames, kept all the time, are
//       captured with this frame's phases (each bin turning with its
//       nearest peak: identity phase locking) and played back and forth
//     a: 0 = not frozen; a new value > 0 captures (the program counts
//        captures: 1, 2, 3 ...)
//     b: fade step per frame (HOP / (fade seconds * SR)): captures,
//        releases and smear on/off crossfade with it
//     c: drift 0..1: each peak's pitch wanders slowly, up to +-15 cents
//     d: smear time in seconds (0: off, the wet sound is silent unless
//        frozen)
//     e: motion 0..1: how far back the frozen playback moves (0: holds the
//        newest frame, 1: all 32)
//     f: speed: frames per frame of that movement (0..4)
//     g: tilt -1..1: the smear time of high frequencies against low ones
//        (-1: highs follow 4x faster, 1: 4x slower)
//     h: morph 0..1: a new capture's partials glide in from the nearest
//        partials of the sound before (within half an octave), over the
//        fade time; a capture that fades out glides towards the new one.
//        0: a plain crossfade, 1: the whole way
//     i: layer 0..1: the level the previous capture keeps under the new
//        one (0: it fades out)
//     j: grain 0..1: a random phase per bin and frame on the stable
//        partials (from clean to noise; no pitch wander)
//     k: pitch ratio of the frozen sound (0.25..4, glided): every peak
//        moves with its bins and turns faster or slower (not the smear)
//   nm_spectral_level(channel, after): how present the wet sound is (0..1,
//     updated every frame; the sound follows a little later: slush aligns
//     it by 192 samples at 1024 points, 640 at 2048, measured). `after` is ignored: pass the
//     output of nm_spectral() so FAUST calls it after the frame
//
// Copyright © 2021 Arev Imer if not mentioned elsewise
// released under GNU GPL v3 or later (see LICENSE)
//

#ifndef nemesis_spectral_h
#define nemesis_spectral_h

#include <stdint.h>

#define NEMESIS_SPECTRAL_MAX 2048
#define NEMESIS_SPECTRAL_CHANNELS 2

namespace nemesis
{
   namespace spectral
   {
      constexpr int MAX = NEMESIS_SPECTRAL_MAX;
      enum Kernel { FREEZE = 0 };

      struct Channel
      {
         float *in = nullptr;    // n: the input ring
         float *out = nullptr;   // n: the overlap-add ring
         int n = 0;              // FFT size (0: not allocated)
         int mask = 0;           // n - 1
         int hop = 0;            // n / 4
         int split = 0;          // hop / 4: samples from a frame's analysis to its synthesis
         int pos = 0;            // ring position: in[pos] the oldest sample, out[pos] the next output
         int count = 0;          // samples since the last frame
         int kernel = 0;
         float p[11] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
         float level = 0;        // FREEZE: how present the wet sound is (0..1), set every frame
      };
      extern Channel channel[NEMESIS_SPECTRAL_CHANNELS];
      extern volatile int requested;   // a size a program asked for (service() applies it)

      // allocates and clears the channels at `size` points (1024 or 2048;
      // RAM2, the level history in PSRAM), false if it can't
      bool begin(int channels, int size = 1024);
      void end();
      int channels();
      int size();
      // the program's sample rate (the time constants are in seconds)
      void setRate(float sampleRate);
      // main loop: a size change a program asked for
      void service();
      // where channel c's buffers are: 'R' RAM2, 'P' PSRAM, '-' none (STATUS)
      char placement(int c);
      // one frame of channel c in two halves (nm_spectral calls them): the
      // analysis every hop, the synthesis hop / 4 samples later
      void frame(int c);
      void synthesis(int c);
      // profile (Teensy): average cycles per frame of each step since the
      // last call (names in profileName), then reset; PROFILE_STEPS steps
      constexpr int PROFILE_STEPS = 8;
      extern const char *const profileName[PROFILE_STEPS];
      void profile(uint32_t *cycles, uint32_t &frames);
   }
}

static inline float nm_spectral(int c, int kernel, int size, float x, float a, float b, float cc, float d, float e,
                                float f, float g, float h, float i, float j, float k)
{
   nemesis::spectral::Channel &s = nemesis::spectral::channel[c];
   if (size != s.n)
      nemesis::spectral::requested = size;   // reallocated in the main loop
   if (!s.in)
      return 0.0f;
   s.in[s.pos] = x;
   float y = s.out[s.pos];
   s.out[s.pos] = 0.0f;
   s.pos = (s.pos + 1) & s.mask;
   s.kernel = kernel;
   s.p[0] = a;
   s.p[1] = b;
   s.p[2] = cc;
   s.p[3] = d;
   s.p[4] = e;
   s.p[5] = f;
   s.p[6] = g;
   s.p[7] = h;
   s.p[8] = i;
   s.p[9] = j;
   s.p[10] = k;
   if (++s.count >= s.hop)
   {
      s.count = 0;
      nemesis::spectral::frame(c);   // the analysis
   }
   else if (s.count == s.split)
      nemesis::spectral::synthesis(c);   // the rest of that frame, in a later block
   return y;
}

static inline float nm_spectral_level(int c, float after)
{
   (void)after;
   return nemesis::spectral::channel[c].level;   // inline: no call per sample
}

#endif
