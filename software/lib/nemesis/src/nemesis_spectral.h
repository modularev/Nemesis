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
//                        float, float, float, float, float, float, float), "nemesis_spectral.h", "");
//   y = spectral(channel, kernel, size, x, a, b, c, d, e, f, g, h, i, j, k, l, m, n);
//
// Per sample nm_spectral() puts x into a ring and returns the next sample
// of the overlap-added output; every HOP = size / 4 samples it runs a frame:
// the last `size` input samples, Hann windowed, through the FFT, a kernel
// on the bins (the parameters a..n as they were when its step ran), the
// inverse FFT, the window again, added into the output ring. The synthesis
// runs in four steps hop / 8 samples apart (kernel, layers, inverse FFT and
// overlap-add), so a frame's work spreads over several blocks: the output
// is size + size / 8 samples late. Channel 1 runs half a hop after channel 0, so their
// frames fall into different audio blocks. CMSIS-DSP's real FFT on the
// Teensy, a plain FFT elsewhere (tests on the Mac).
//
// size: 512, 1024, 2048 or 4096 points. A program can change it while
// running (a setting): the library reallocates in the main loop
// (service()), the output is silent for that moment. A size that doesn't
// fit keeps the old one (refused) until another one is asked for. At 4096
// two channels need more fast memory than there is: only channel 0 runs,
// the program gives it both inputs (mono); its bins are spread over the
// stereo field (fixed random places: mid + side, channel 0 left, channel 1
// returns the right), so a program's width still works.
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
//     c: drift 0..1: each peak's pitch wanders slowly, up to +-50 cents,
//        and its level, up to +-4 dB (both with drift^2)
//     d: smear time in seconds (0: off, the wet sound is silent unless
//        frozen)
//     e: scatter 0..1: every bin plays the level and (by as much as
//        scatter) the frequency of a random frame of the capture's newest
//        2 .. 32 (0: the averaged hold), the level smoothed by
//     f: the scatter's smoothing time in seconds
//     g: tilt -1..1: the smear time of high frequencies against low ones
//        (-1: highs follow 4x faster, 1: 4x slower)
//     h: morph 0..1: a new capture's partials glide in from the nearest
//        partials of the sound before (within half an octave), over the
//        fade time; a capture that fades out glides towards the new one.
//        0: a plain crossfade, 1: the whole way
//     i: layer 0..1: the level the previous capture keeps under the new
//        one (0: it fades out)
//     j: grain 0..1: a random phase and level per bin and frame (from
//        clean to noise; the components move within their bins)
//     k: pitch ratio of the frozen sound (0.25..4, glided): every peak
//        moves with its bins and turns faster or slower (not the smear)
//     l: smear amount 0..1: the level the smear fades to (d = 0: no
//        inertia, the input resynthesized)
//     m: purity 0..1: only partials within -90 dB (0) .. -6 dB (1) of the
//        loudest sound (smear and frozen)
//     n: lock 0..1: 1 = the frozen bins turn with their peak; lower, each
//        moves towards its own frequency and its phase wanders (diffuse)
//   A capture holds the levels averaged over its last ~100 ms. With morph
//   1 its partials start at the phases of the old ones they glide from:
//   the fade (b) then interpolates between the two
//   nm_spectral_level(channel, after): how present the wet sound is (0..1,
//     updated every frame with the overlap-add; the sound follows a little
//     later: slush aligns it by 7/16 of the size, measured). `after` is ignored: pass the
//     output of nm_spectral() so FAUST calls it after the frame
//
// Copyright © 2021 Arev Imer if not mentioned elsewise
// released under GNU GPL v3 or later (see LICENSE)
//

#ifndef nemesis_spectral_h
#define nemesis_spectral_h

#include <stddef.h>
#include <stdint.h>

#define NEMESIS_SPECTRAL_MAX 4096
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
         int split = 0;          // hop / 8: samples between a frame's analysis and its synthesis steps
         int next = 0;           // count of the next synthesis step
         int pos = 0;            // ring position: in[pos] the oldest sample, out[pos] the next output
         int count = 0;          // samples since the last frame
         int kernel = 0;
         float p[14] = {0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 1, 0, 1};
         float level = 0;        // FREEZE: how present the wet sound is (0..1), set every frame
         float y = 0;            // the last output sample
         float *out2 = nullptr;  // mono at 4096: the right channel's overlap-add ring
         float y2 = 0;           // and its last output sample
         int follow = -1;        // >= 0: this channel only plays that one's output (mono at 4096)
      };
      extern Channel channel[NEMESIS_SPECTRAL_CHANNELS];
      extern volatile int requested;   // a size a program asked for (service() applies it)
      extern volatile int refused;     // a size that didn't fit (not asked for again)

      // allocates and clears the channels at `size` points (512..4096; the
      // arena's rest and RAM2, the level history in PSRAM), false if it
      // can't. At 4096 only channel 0 runs: the others play its output
      bool begin(int channels, int size = 1024);
      void end();
      int channels();
      int size();
      // the program's sample rate (the time constants are in seconds)
      void setRate(float sampleRate);
      // main loop: a size change a program asked for
      void service();
      // channels that run their own frames (fewer than channels() when mono),
      // and the bytes in RAM1 (the arena's rest) and RAM2 (STATUS)
      int running();
      void memoryUse(size_t &dtcm, size_t &ram2);
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
                                float f, float g, float h, float i, float j, float k, float l,
                                float m, float n)
{
   nemesis::spectral::Channel &s = nemesis::spectral::channel[c];
   if (size != s.n && size != nemesis::spectral::refused)
      nemesis::spectral::requested = size;   // reallocated in the main loop
   if (s.follow >= 0)
   {
      const nemesis::spectral::Channel &o = nemesis::spectral::channel[s.follow];
      return o.out2 ? o.y2 : o.y;   // mono (4096 points): the other side of its spread
   }
   if (!s.in)
      return 0.0f;
   s.in[s.pos] = x;
   float y = s.out[s.pos];
   s.out[s.pos] = 0.0f;
   if (s.out2)
   {
      s.y2 = s.out2[s.pos];
      s.out2[s.pos] = 0.0f;
   }
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
   s.p[11] = l;
   s.p[12] = m;
   s.p[13] = n;
   if (++s.count >= s.hop)
   {
      s.count = 0;
      s.next = s.split;
      nemesis::spectral::frame(c);   // the analysis
   }
   else if (s.count == s.next)
   {
      s.next += s.split;
      nemesis::spectral::synthesis(c);   // a step of that frame's synthesis, in later blocks
   }
   s.y = y;
   return y;
}

static inline float nm_spectral_level(int c, float after)
{
   (void)after;
   const nemesis::spectral::Channel &s = nemesis::spectral::channel[c];
   return s.follow >= 0 ? nemesis::spectral::channel[s.follow].level : s.level;   // inline: no call per sample
}

#endif
