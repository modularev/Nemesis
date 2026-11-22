// SPDX-License-Identifier: GPL-3.0-or-later
//
// Library for the programmable Eurorack DSP Module NEMESIS
//
// nemesis_spectral.cpp - spectral processing (a phase vocoder), see
// nemesis_spectral.h
//
// Copyright © 2021 Arev Imer if not mentioned elsewise
// released under GNU GPL v3 or later (see LICENSE)
//

#include "nemesis_spectral.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#if defined(ARDUINO) && defined(__arm__)
#include <Arduino.h>
#include <AudioStream.h>
#include <arm_math.h>
#include <arm_common_tables.h>
#include <arm_const_structs.h>
#define NM_SPECTRAL_CMSIS 1
namespace nemesis { void *arenaAllocate(size_t size); size_t arenaFree(); }
#else
#define FLASHMEM
#endif

namespace nemesis
{
   namespace spectral
   {
      Channel channel[NEMESIS_SPECTRAL_CHANNELS];
      volatile int requested = 0;
      volatile int refused = 0;
      const char *const profileName[PROFILE_STEPS] = {"window+fft", "levels+energy", "peaks", "turns",
                                                       "smear", "frozen", "inverse+ola", "corrections"};

      namespace
      {
         constexpr float TAU = 6.28318530718f;   // (Arduino defines TWO_PI as a macro)
         constexpr int M = 32;                   // frames of level history (170 ms at 1024 points, 48 kHz)

         int N = 1024;       // FFT size (begin())
         int HOP = 256;      // N / 4
         int K = 513;        // bins: N / 2 + 1
         int MASK = 1023;    // N - 1

         // FREEZE keeps the levels of the last M frames all the time; a
         // capture takes them into a layer (by swapping buffers: no copy)
         // and plays them back and forth (motion) with the phases of the
         // capture frame. Phase turns per hop as unit vectors: this frame
         // times the conjugate of the previous one, no atan2/sin/cos per bin;
         // every bin takes the turn of the nearest spectral peak (identity
         // phase locking, Laroche & Dolson 1999: the bins of one partial
         // stay together instead of drifting apart into a metallic smear).
         // A layer keeps each peak's frequency (in bins) and plays it at a
         // ratio (pitch x drift x morph glide): the peak's bins move
         // together by the nearest whole number of bins, its phase turns at
         // the new frequency (Laroche & Dolson's peak shifting)
         struct Layer
         {
            float *frames = nullptr;   // M * K (level, frequency) pairs: the history's ring as it was at the capture (PSRAM)
            int start = 0;             // its oldest frame
            int valid = M;             // its newest frames that are real (the rest is an older capture's)
            float *z = nullptr;        // N: phase per bin as a unit vector, packed as the spectrum
            float *freq = nullptr;     // K: at a peak's bin: its frequency in bins (from its turn)
            int *peakOf = nullptr;     // K: each bin's peak (its bins move and turn together)
            float *tonal = nullptr;    // K: 1 = a stable partial (locked phase), 0 = random phase each frame
            float *wander = nullptr;   // K: drift per peak (at its bin), -1..1, gliding to
            float *aim = nullptr;      // K: its random target (a new one every ~0.34 s)
            float *bendA = nullptr;    // K: morph glide per peak (at its bin) in octaves, from bendA
            float *bendB = nullptr;    // K: to bendB, while u goes from 0 to 1
            float *life = nullptr;     // K: level wander per peak (at its bin), -1..1, gliding to
            float *lifeAim = nullptr;  // K: its random target
            float maxPeak = 0;         // the loudest peak's level of the last frame (purity)
            float *level = nullptr;    // K: each bin's level as it plays (scatter smooths it)
            float *wander2 = nullptr;  // K: each bin's phase wander (lock < 1), radians, pulled back to 0
            float u = 1;               // glide progress (at the fade's speed)
            bool filled = false;       // holds a capture (only then held under the next one)
            float g = 0, target = 0;   // fade
            float pos = M - 1, dir = -1;   // place in the frames (motion)
            float norm = 1;            // level match: gain towards the input's energy at the capture
            float targetE = 0;         // that energy (per frame)
            float targetT = 0;         // the input's energy per sample at the capture
         };

         // the smear (see smear()). (Gliding the turns between frames left
         // the frames out of step with the waveform inside them: a buzz,
         // sidebands at the frame rate 34 dB down: a bin's turn changes only
         // when it learns a new frequency)
         struct Smear
         {
            float *mag = nullptr;      // K
            float *z = nullptr;        // N
            float *weight = nullptr;   // K: tilt per bin (follow speed relative to the middle)
            float *freq = nullptr;     // K: per bin the frequency (bins) it runs at, learned from its input peak
            float *tonal = nullptr;    // K: 1 = a stable partial (coherent phase), 0 = random phases
            float *tv = nullptr;       // K: the input's tonal test, smoothed (tonal decides with hysteresis)
            float *fresh = nullptr;    // K: 1 while the input carries the bin (it may learn), 0 for a smeared tail
            float *last = nullptr;     // K: the input's level of the previous frame
            float maxPeak = 0;         // the loudest smeared peak of the last frame (purity)
            float tilt = 2.0f;         // the tilt weight was made for (2: none yet)
            float g = 0;               // fade (smear on/off, d > 0)
            bool primed = false;       // state taken from the input
            float corr = 1;            // output level correction (smoothed levels lose energy)
            float planned = 0;         // energy of the levels it plays (sum of mag^2), averaged
            float played = 0;          // the output's energy per sample without corr, averaged
         };

         struct State
         {
            float *time = nullptr;   // N: the windowed frame (FFT input, then inverse output)
            float *spec = nullptr;   // N: this frame's spectrum, CMSIS packing: [0] DC, [1] Nyquist, then re, im of bins 1..N/2-1
            float *prev = nullptr;   // N: the previous frame's spectrum
            float *syn = nullptr;    // N: the spectrum to resynthesize
            float *turn = nullptr;   // N: this frame's phase turns, locked to the peaks
            float *lastTurn = nullptr;   // N: the previous frame's (stability: tonal or not)
            float *tonalNow = nullptr;   // K: this frame's tonal flags (per peak, see tonality())
            float *binF = nullptr;   // K: this frame's frequency of every bin (its own turn)
            float *avg = nullptr;    // K: every bin's level averaged over ~150 ms, in log2 (what a capture holds)
            float *syn2 = nullptr;   // N: mono at 4096: the side spectrum (the bins spread over the stereo field)
            float *pan = nullptr;    // K: each output bin's side share, -0.9 .. 0.9 (fixed, random)
            float *mag = nullptr;    // K: this frame's levels (RAM2; copied into the PSRAM history:
                                     // read from there, the peak search took 115k cycles a frame)
            float *hist = nullptr;   // M * K: each bin's level and frequency (bins) of the last M frames, a ring (PSRAM)
            int histPos = 0;         // the ring's next frame
            int histValid = 0;       // frames written into the history's buffer since it became the history
            int *peak = nullptr;     // K (scratch): each bin's nearest peak
            Layer layer[2];
            Smear smear;
            int current = 0;
            float captured = 0;      // the a value of the latest capture
            int waiting = -1;        // a layer fading out quickly for a capture into it
            float ratio = 1;         // pitch ratio, glided (parameter k)
            uint32_t rng = 1;
            float energy = 0;        // the input's energy per frame, averaged over ~0.7 s
            float energyT = 0;       // the same per input sample (time domain)
            float corr = 1;          // freeze output level correction, kept from capture to capture
            // every array its own allocation (they fit into the holes of a
            // heap fragmented by other programs; one block per channel
            // didn't and went to PSRAM, where a 2048 frame took 220 % of a
            // 128 sample block: stutter, and with both channels the main
            // loop starved, watchdog reset)
            static constexpr int MAX_PARTS = 56;
            void *parts[MAX_PARTS] = {};
            int nparts = 0;
            void *slow = nullptr;    // PSRAM: the level history
            // the synthesis runs in steps (see synthesis()): the next one
            // (0: none), and what the first step leaves for the others
            int step = 0;
            bool active = false;     // something sounds: the layers or the smear
            float gf = 0, ws = 0;    // the frozen sound's share, the smear's
            float level = 0;         // published with the overlap-add (s.level)
            float cents = 0, spread = 0, share = 1, life = 0, grain = 0, purity = 0, frozenShape = 1;
            float scatter = 0, scatterA = 1, lock = 1;
         };

         State state[NEMESIS_SPECTRAL_CHANNELS];
         float *window = nullptr;   // N: periodic Hann (Hann^2 at a hop of N/4 sums to 1.5)
         // shared by the channels: the windowed frame and the spectrum to
         // resynthesize are scratch within one call of frame() or
         // synthesis(), and the channels' calls never overlap
         float *sharedTime = nullptr, *sharedSyn = nullptr;
         // Memory: the part of the library's arena (RAM1, DTCM: single cycle,
         // no cache) the program left, taken when it starts and kept while
         // it runs (size changes refill it), then the heap (RAM2). Nothing
         // a frame touches may be in PSRAM: rings and spectra there made a
         // 4096 frame take 127-163 % of an audio block
         uint8_t *region = nullptr;
         size_t regionSize = 0, regionUsed = 0;
         size_t inDTCM = 0, inRAM2 = 0;   // bytes (STATUS)
         inline bool inRegion(const void *p)
         {
            return region && p >= (const void *)region && p < (const void *)(region + regionSize);
         }
         // a zeroed array, nullptr if there is no room
         void *takeMem(size_t bytes)
         {
            size_t start = (regionUsed + 31) & ~(size_t)31;
            void *m;
            if (region && start <= regionSize && bytes <= regionSize - start)
            {
               m = region + start;
               regionUsed = start + bytes;
               inDTCM += bytes;
            }
            else if ((m = malloc(bytes)) != nullptr)
               inRAM2 += bytes;
            if (m)
               memset(m, 0, bytes);
            return m;
         }
         void freeMem(void *p)
         {
            if (!inRegion(p))
               free(p);
         }
         // allocations of all channels: the window, the scratch, copies of FFT tables
         void *common[8] = {};
         int ncommon = 0;
         void *takeCommon(size_t bytes)
         {
            void *m = ncommon < 8 ? takeMem(bytes) : nullptr;
            if (m)
               common[ncommon++] = m;
            return m;
         }
         float gain = 1.0f;   // inverse FFT and overlap-add back to unity, measured in begin()
         float fps = 48000.0f / 256;   // frames per second (setRate): time constants in seconds, not frames
         float rate = 48000.0f;
         int used = 0;
         // profile: cycles per step, summed
         volatile uint32_t profSum[PROFILE_STEPS];
         volatile uint32_t profFrames = 0;
#ifdef NM_SPECTRAL_CMSIS
         inline uint32_t now() { return ARM_DWT_CYCCNT; }
#else
         inline uint32_t now() { return 0; }
#endif
         uint32_t profMark = 0;
         inline void lap(int step)
         {
            uint32_t t = now();
            profSum[step] += t - profMark;
            profMark = t;
         }

         // a one-pole coefficient per frame for a time constant in seconds
         inline float perFrame(float seconds) { return 1.0f - expf(-1.0f / (seconds * fps)); }

#ifdef NM_SPECTRAL_CMSIS
         arm_rfft_fast_instance_f32 fft;
         // by hand: arm_rfft_fast_init_f32() refers to the tables of every
         // size, and the linker kept ~70 kB of them in RAM1. 1024 and 2048
         // use their tables in RAM1 (DTCM). 512's and 4096's stay in flash
         // (teensy41_flash.ld: RAM1 has no room for 4096's 40 kB) and are
         // copied to RAM2 while that size runs: read from flash, the tables
         // made a 2048 frame take 150 % of a 64 sample block
         bool setupFFT(int n)
         {
            const arm_cfft_instance_f32 *c;
            const float32_t *rfft;
            bool copy = false;
            switch (n)
            {
            case 512: c = &arm_cfft_sR_f32_len256; rfft = twiddleCoef_rfft_512; copy = true; break;
            case 1024: c = &arm_cfft_sR_f32_len512; rfft = twiddleCoef_rfft_1024; break;
            case 2048: c = &arm_cfft_sR_f32_len1024; rfft = twiddleCoef_rfft_2048; break;
            case 4096: c = &arm_cfft_sR_f32_len2048; rfft = twiddleCoef_rfft_4096; copy = true; break;
            default: return false;
            }
            fft.Sint = *c;
            fft.fftLenRFFT = n;
            fft.pTwiddleRFFT = (float32_t *)rfft;
            if (copy)
            {
               size_t tw = (size_t)c->fftLen * 2 * sizeof(float), br = (size_t)c->bitRevLength * sizeof(uint16_t);
               float *t = (float *)takeCommon(tw);
               uint16_t *b = (uint16_t *)takeCommon(br);
               float *r = (float *)takeCommon(n * sizeof(float));
               if (!t || !b || !r)
                  return false;
               memcpy(t, c->pTwiddle, tw);
               memcpy(b, c->pBitRevTable, br);
               memcpy(r, rfft, n * sizeof(float));
               fft.Sint.pTwiddle = t;
               fft.Sint.pBitRevTable = b;
               fft.pTwiddleRFFT = r;
            }
            return true;
         }
         // both use their input as scratch
         void forward(float *time, float *spec) { arm_rfft_fast_f32(&fft, time, spec, 0); }
         void inverse(float *spec, float *time) { arm_rfft_fast_f32(&fft, spec, time, 1); }
#else
         // a plain complex FFT for the tests on the Mac, with CMSIS's packing
         float re[MAX], im[MAX];
         bool setupFFT(int n) { return n == 512 || n == 1024 || n == 2048 || n == 4096; }
         void cfft(bool inv)
         {
            for (int i = 1, j = 0; i < N; i++)
            {
               int bit = N >> 1;
               for (; j & bit; bit >>= 1)
                  j ^= bit;
               j ^= bit;
               if (i < j)
               {
                  float t = re[i]; re[i] = re[j]; re[j] = t;
                  t = im[i]; im[i] = im[j]; im[j] = t;
               }
            }
            for (int len = 2; len <= N; len <<= 1)
            {
               double ang = (inv ? 2.0 : -2.0) * M_PI / len;
               for (int i = 0; i < N; i += len)
                  for (int k = 0; k < len / 2; k++)
                  {
                     float wr = (float)cos(ang * k), wi = (float)sin(ang * k);
                     float ur = re[i + k], ui = im[i + k];
                     float vr = re[i + k + len / 2] * wr - im[i + k + len / 2] * wi;
                     float vi = re[i + k + len / 2] * wi + im[i + k + len / 2] * wr;
                     re[i + k] = ur + vr; im[i + k] = ui + vi;
                     re[i + k + len / 2] = ur - vr; im[i + k + len / 2] = ui - vi;
                  }
            }
         }
         void forward(float *time, float *spec)
         {
            for (int i = 0; i < N; i++) { re[i] = time[i]; im[i] = 0; }
            cfft(false);
            spec[0] = re[0];
            spec[1] = re[N / 2];
            for (int k = 1; k < N / 2; k++) { spec[2 * k] = re[k]; spec[2 * k + 1] = im[k]; }
         }
         void inverse(float *spec, float *time)
         {
            re[0] = spec[0]; im[0] = 0;
            re[N / 2] = spec[1]; im[N / 2] = 0;
            for (int k = 1; k < N / 2; k++)
            {
               re[k] = spec[2 * k]; im[k] = spec[2 * k + 1];
               re[N - k] = re[k]; im[N - k] = -im[k];
            }
            cfft(true);
            for (int i = 0; i < N; i++) time[i] = re[i] / N;
         }
#endif

         inline float noise(uint32_t &r)   // -1 .. 1
         {
            r ^= r << 13;
            r ^= r >> 17;
            r ^= r << 5;
            return (int32_t)r * (1.0f / 2147483648.0f);
         }

         inline float clamp(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }

         float unitTable[256][2];   // random phases: cos, sin of 256 angles (begin())

         // cos and sin of q quarter turns (q * pi/2), any q: polynomials on
         // +-pi/4 around the nearest quarter (error ~3e-7, no libm call)
         inline void quarterTurns(double q, float &c, float &s)
         {
            q -= 4.0 * floor(q * 0.25);
            int n = (int)(q + 0.5);
            float r = (float)(q - n) * 1.5707963f;
            float r2 = r * r;
            float s0 = r * (1.0f - r2 * (1.0f / 6 - r2 * (1.0f / 120 - r2 * (1.0f / 5040))));
            float c0 = 1.0f - r2 * (0.5f - r2 * (1.0f / 24 - r2 * (1.0f / 720 - r2 * (1.0f / 40320))));
            switch (n & 3)
            {
            case 0: c = c0; s = s0; break;
            case 1: c = -s0; s = c0; break;
            case 2: c = -c0; s = -s0; break;
            default: c = s0; s = -c0; break;
            }
         }

         // atan2 without libm (newlib's took ~1.5 % per frame in the smear):
         // octant reduction and a polynomial, error < 2e-4 rad (0.003 Hz at 2048 points)
         inline float fastAtan2(float y, float x)
         {
            float ax = fabsf(x), ay = fabsf(y);
            float mx = ax > ay ? ax : ay, mn = ax > ay ? ay : ax;
            if (mx <= 0.0f)
               return 0.0f;
            float a = mn / mx, s = a * a;
            float r = ((-0.0464964749f * s + 0.15931422f) * s - 0.327622764f) * s * a + a;
            if (ay > ax) r = 1.57079637f - r;
            if (x < 0.0f) r = 3.14159274f - r;
            return y < 0.0f ? -r : r;
         }

         // a peak's frequency in bins from its turn per hop of N/4 (a bin
         // turns pi/2 per bin and hop: the turn against bin p's own, within
         // +-2 bins)
         inline float peakFrequency(int p, float tr, float ti)
         {
            static const float centre[4][2] = {{1, 0}, {0, 1}, {-1, 0}, {0, -1}};
            float er = centre[p & 3][0], ei = centre[p & 3][1];
            float cr = tr * er + ti * ei, ci = ti * er - tr * ei;   // turn * conj(i^p)
            return p + fastAtan2(ci, cr) * (2.0f / 3.14159265f);
         }

         inline float bendNow(const Layer &L, int p)
         {
            return L.u >= 1.0f ? L.bendB[p] : L.bendA[p] + (L.bendB[p] - L.bendA[p]) * L.u;
         }

         // this frame's nearest peak of every bin (two passes: the last peak
         // below, the next one above), from levels mag[0..K-1]
         FLASHMEM void findPeaks(const float *mag, int *peak)
         {
            int last = -1;
            for (int k = 1; k < N / 2; k++)
            {
               bool isPeak = mag[k] > mag[k - 1] && mag[k] >= mag[k + 1] && mag[k] > 1e-9f;
               if (isPeak) last = k;
               peak[k] = last;
            }
            int next = -1;
            for (int k = N / 2 - 1; k >= 1; k--)
            {
               bool isPeak = mag[k] > mag[k - 1] && mag[k] >= mag[k + 1] && mag[k] > 1e-9f;
               if (isPeak) next = k;
               int below = peak[k];
               if (below < 0 || (next >= 0 && next - k < k - below)) peak[k] = next;
               if (peak[k] < 1) peak[k] = k;
            }
         }

         // every bin takes its peak's phase turn since the previous frame
         // (the peak's spectrum times the conjugate of the previous one): f.turn.
         // Only the peaks' turns are computed (a peak's bins are contiguous)
         FLASHMEM void lockedTurns(State &f)
         {
            static const float expected[4][2] = {{1, 0}, {0, 1}, {-1, 0}, {0, -1}};   // i^k: a bin's turn per hop of N/4
            int last = -1;
            float tr = 1.0f, ti = 0.0f;
            for (int k = 1; k < N / 2; k++)
            {
               int p = f.peak[k];
               if (p != last)
               {
                  last = p;
                  float cr = f.spec[2 * p], ci = f.spec[2 * p + 1];
                  float pr = f.prev[2 * p], pi = f.prev[2 * p + 1];
                  float dr = cr * pr + ci * pi, di = ci * pr - cr * pi;
                  float dm = sqrtf(dr * dr + di * di);
                  if (dm > 1e-20f) { float inv = 1.0f / dm; tr = dr * inv; ti = di * inv; }
                  else { tr = expected[p & 3][0]; ti = expected[p & 3][1]; }
               }
               f.turn[2 * k] = tr;
               f.turn[2 * k + 1] = ti;
            }
         }

         // tonal: a stable partial, decided per peak for all its bins (f.tonalNow).
         // Where partials are closer than the window resolves (a low chord
         // at 1024 or 2048 points), a bin holds a beating mixture: held on
         // with its turn, it played tones that weren't there (178 and 299 Hz
         // for 196/247/294); with a new random phase every frame it keeps its
         // level as texture instead. A peak is tonal when its turn is the
         // same as one frame earlier (within ~18 degrees) and puts its
         // frequency within 0.6 bins of the peak bin (a bin's phase turns
         // pi/2 per bin and hop of N/4)
         FLASHMEM void tonality(State &f)
         {
            for (int k = 1; k < N / 2; k++)
            {
               if (f.peak[k] != k)
                  continue;
               float tr = f.turn[2 * k], ti = f.turn[2 * k + 1];
               float stab = tr * f.lastTurn[2 * k] + ti * f.lastTurn[2 * k + 1];
               // within 0.6 bins of the bin centre: the angle to the centre's
               // turn (i^k) below 0.6 * pi/2, i.e. their dot product above
               // cos(0.94) (no atan2: 100k cycles a frame at 2048 points)
               static const float centre[4][2] = {{1, 0}, {0, 1}, {-1, 0}, {0, -1}};
               float near = tr * centre[k & 3][0] + ti * centre[k & 3][1];
               f.tonalNow[k] = (stab > 0.95f && near > 0.5878f) ? 1.0f : 0.0f;
            }
            for (int k = 1; k < N / 2; k++)
               f.tonalNow[k] = f.tonalNow[f.peak[k]];
         }

         // morph: each peak of `to` gets the nearest peak of `from` (in
         // octaves, within half an octave): its distance in `dist` (at the
         // peak's bin, octaves from `to` to `from`; 0 without a partner).
         // Peaks as frequencies in ascending order (bins), sorted by
         // construction; fromNow: from's frequencies as they sound now
         FLASHMEM void nearest(const float *fromF, int nFrom, const float *toF, const int *toBin, int nTo, float *dist,
                               int *partner = nullptr)
         {
            int j = 0;
            for (int i = 0; i < nTo; i++)
            {
               float t = toF[i];
               while (j + 1 < nFrom && fabsf(fromF[j + 1] - t) <= fabsf(fromF[j] - t))
                  j++;
               float d = 0.0f;
               int q = -1;
               if (nFrom > 0 && t > 0.5f && fromF[j] > 0.5f)
               {
                  d = log2f(fromF[j] / t);
                  q = j;
                  if (fabsf(d) > 0.5f)
                  {
                     d = 0.0f;
                     q = -1;
                  }
               }
               dist[toBin[i]] = d;
               if (partner)
                  partner[i] = q;
            }
         }

         // the history into layer l (silent: freeze() fades it out first),
         // with this frame's phases, turns and peak frequencies. The layer
         // that played until now (prev, -1: none) is held at `hold` or fades
         // out; with morph the new peaks glide in from its peaks and, when it
         // fades out, its peaks glide towards the new ones
         FLASHMEM void capture(State &f, float a, int l, float morph, float hold)
         {
            Layer &L = f.layer[l];
            int prev = l != f.current && f.layer[f.current].g > 0.0f ? f.current : -1;
            // the history becomes the layer's frames, the history goes on in
            // the layer's old buffer: no copy (copying 66 kB into PSRAM took
            // longer than an audio block: a late block, a click at every
            // freeze). Frames older than the last capture in that buffer are
            // stale until the history has filled it again.
            float *b = L.frames;
            L.frames = f.hist;
            f.hist = b;
            L.start = f.histPos;   // the oldest frame of the ring
            // only the frames written since the buffer became the history are
            // this sound: the rest are the levels of an older capture (they
            // played with this capture's phases: a re-freeze within 32 frames
            // of the last one dropped out and warbled). Motion stays within
            // the real ones
            L.valid = f.histValid > 0 ? f.histValid : 1;
            f.histValid = 0;
            for (int k = 1; k < N / 2; k++)
            {
               float cr = f.spec[2 * k], ci = f.spec[2 * k + 1];
               float m = sqrtf(cr * cr + ci * ci);
               if (m > 1e-12f) { L.z[2 * k] = cr / m; L.z[2 * k + 1] = ci / m; }
               else { L.z[2 * k] = 1.0f; L.z[2 * k + 1] = 0.0f; }
               L.peakOf[k] = f.peak[k];
               L.tonal[k] = f.tonalNow[k];
               L.bendA[k] = L.bendB[k] = 0.0f;
               L.wander[k] = L.aim[k] = 0.0f;
               L.life[k] = L.lifeAim[k] = 0.0f;
               if (f.peak[k] == k)
                  L.freq[k] = f.tonalNow[k] > 0.5f ? peakFrequency(k, f.turn[2 * k], f.turn[2 * k + 1]) : (float)k;
            }
            L.u = 1.0f;
            L.filled = true;
            L.maxPeak = 0.0f;
            // the held levels: every bin's level averaged over ~150 ms
            // (geometric, kept up to date every frame on chip: averaging the
            // history's frames from PSRAM at the capture took most of a
            // block, a click), in the newest frame of the capture: the
            // sound's steady timbre rather than one window's moment
            {
               float *newest = L.frames + ((L.start + M - 1) % M) * 2 * K;
               for (int k = 0; k < K; k++)
                  newest[2 * k] = L.level[k] = k > 0 && k < N / 2 ? exp2f(f.avg[k]) : 0.0f;
            }
            if (prev >= 0 && morph > 0.0f)
            {
               // the peaks of both as lists (scratch: syn and time are free
               // until the synthesis below)
               Layer &P = f.layer[prev];
               const int Q = N / 4;   // at most N/4 peaks
               float *nf = f.syn, *pf = f.time;
               int *nb = (int *)(f.syn + Q), *pb = (int *)(f.time + Q);
               int nn = 0, np = 0;
               for (int k = 1; k < N / 2; k++)
               {
                  if (L.peakOf[k] == k && nn < Q) { nf[nn] = L.freq[k]; nb[nn++] = k; }
                  if (P.peakOf[k] == k && np < Q) { pf[np] = P.freq[k] * exp2f(bendNow(P, k)); pb[np++] = k; }
               }
               // new peaks: from the old pitch to their own
               int *partner = (int *)(f.syn + 2 * Q);
               nearest(pf, np, nf, nb, nn, L.bendA, partner);
               for (int i = 0; i < nn; i++)
                  L.bendA[nb[i]] *= morph;
               // a new tonal peak that starts at an old one's pitch also
               // starts at its phase (its bins turned together): both play
               // the same frequency in phase while they glide, so the
               // crossfade interpolates their level instead of beating
               // (two copies of a partial with unrelated phases cancelled
               // in the middle of a crossfade)
               if (morph >= 1.0f)
                  for (int i = 0; i < nn; i++)
                  {
                     int p = nb[i], q = partner[i] >= 0 ? pb[partner[i]] : -1;
                     if (q < 0 || L.tonal[p] < 0.5f || P.tonal[q] < 0.5f)
                        continue;
                     float ar = P.z[2 * q], ai = P.z[2 * q + 1], br = L.z[2 * p], bi = L.z[2 * p + 1];
                     float rr = ar * br + ai * bi, ri = ai * br - ar * bi;   // old * conj(new)
                     if ((q - p) & 1)
                     {
                        // the same phase at the windows' centre: pi per bin
                        rr = -rr;
                        ri = -ri;
                     }
                     for (int k = p; k >= 1 && L.peakOf[k] == p; k--)
                     {
                        float zr = L.z[2 * k], zi = L.z[2 * k + 1];
                        L.z[2 * k] = zr * rr - zi * ri;
                        L.z[2 * k + 1] = zr * ri + zi * rr;
                     }
                     for (int k = p + 1; k < N / 2 && L.peakOf[k] == p; k++)
                     {
                        float zr = L.z[2 * k], zi = L.z[2 * k + 1];
                        L.z[2 * k] = zr * rr - zi * ri;
                        L.z[2 * k + 1] = zr * ri + zi * rr;
                     }
                  }
               L.u = 0.0f;
               if (hold <= 0.0f)
               {
                  // the old one fades out: from where it sounds now towards
                  // the new peaks (pf is its frequencies with the bend)
                  for (int i = 0; i < np; i++)
                     P.bendA[pb[i]] = bendNow(P, pb[i]);
                  nearest(nf, nn, pf, pb, np, P.bendB);   // octaves from its sound now to a new peak
                  for (int i = 0; i < np; i++)
                     P.bendB[pb[i]] = P.bendA[pb[i]] + morph * P.bendB[pb[i]];
                  P.u = 0.0f;
               }
               memset(f.syn, 0, N * sizeof(float));
            }
            // level: the held levels as they are (their energy: targetE);
            // the gain only keeps scatter's played levels there, and the
            // output correction steers the sound to what those levels mean
            // in time (the input's ratio of time to spectral energy), not
            // to the input's energy: steered to the input, the held sustain
            // took the level of a piano's attacks
            double e0 = 0;
            for (int k = 1; k < N / 2; k++)
               e0 += (double)L.level[k] * L.level[k];
            L.targetE = (float)e0;
            L.targetT = f.energy > 1e-20f ? (float)(f.energyT * e0 / f.energy) : 0.0f;
            L.norm = 1.0f;
            L.g = 0.0f;
            L.pos = M - 1;
            L.dir = -1.0f;
            f.current = l;
            f.captured = a;
         }

         // purity: a peak's gain against the loudest of the last frame: 1
         // above the threshold, 0 below half of it (a soft 6 dB edge, no
         // flicker). The threshold is -90 dB at 0 .. -1 dB at 1 (only the
         // strongest partial: a simpler, 'electronic' timbre)
         inline float purityThreshold(float purity, float maxPeak)
         {
            return purity > 0.0f ? maxPeak * exp2f((-90.0f + 89.0f * purity) * 0.16609640f) : 0.0f;   // dB / 20 * log2(10)
         }
         // and above 0.7 every partial narrows: only bins within `width` of
         // its peak (13 at 0.7 .. 1 at 1), a soft edge of one bin
         inline float purityWidth(float purity)
         {
            return purity > 0.7f ? 1.0f + 12.0f * (1.0f - (purity - 0.7f) / 0.3f) : 1e9f;
         }
         inline float widthGain(int k, int p, float width)
         {
            float g = width + 1.0f - (float)(k > p ? k - p : p - k);
            return g >= 1.0f ? 1.0f : (g <= 0.0f ? 0.0f : g);
         }
         inline float purityGain(float level, float thr)
         {
            if (thr <= 0.0f || level >= thr)
               return 1.0f;
            float x = (level - 0.5f * thr) / (0.5f * thr);
            return x <= 0.0f ? 0.0f : x;
         }

         // the smear: every bin is an oscillator that runs on at the
         // frequency it learned, with a level that follows the input with
         // inertia. A bin learns (its frequency from its input peak, its phase
         // offset to that peak's running phase: identity phase locking,
         // Laroche & Dolson) only while the input carries it (its level near
         // the smeared one), steady (not falling) and tonal; the peak itself
         // never snaps to the input. Bins that aren't stable partials get a
         // random phase each frame.
         // Before, a bin took the input's phase while the input carried it and
         // ran on by itself otherwise: on a piano's beating partials the two
         // switched every ~150 ms (7 % of the strong bins' frames), each
         // switch a small jump: beads and steps along the partials. Locked to
         // the input's nearest peak, a note's tail jumped to the next note's
         // pitch (220 Hz played on at 237 Hz after 330 Hz came)
         FLASHMEM void smear(State &f, const float *mag, float seconds, float tilt, float w, float purity)
         {
            Smear &S = f.smear;
            if (!S.primed)
            {
               // start from the input as it is
               for (int k = 1; k < N / 2; k++)
               {
                  S.mag[k] = mag[k];
                  S.fresh[k] = 1.0f;
                  S.last[k] = mag[k];
                  S.tv[k] = f.tonalNow[k];
                  S.tonal[k] = f.tonalNow[k];
                  S.freq[k] = (float)k;
                  float cr = f.spec[2 * k], ci = f.spec[2 * k + 1];
                  float m = mag[k];
                  if (m > 1e-12f) { S.z[2 * k] = cr / m; S.z[2 * k + 1] = ci / m; }
                  else { S.z[2 * k] = 1.0f; S.z[2 * k + 1] = 0.0f; }
               }
               S.primed = true;
            }
            if (tilt != S.tilt)
            {
               // a bin's follow speed against the middle (log frequency):
               // tilt 1: highs 4x slower, lows 4x faster; recomputed only
               // when the tilt changes
               float half = 0.5f * log2f((float)(N / 2));
               for (int k = 1; k < N / 2; k++)
                  S.weight[k] = powf(4.0f, -tilt * (log2f((float)k) - half) / half);
               S.tilt = tilt;
            }
            // per frame, at the middle (seconds 0: no inertia, the input as
            // it is: a live pitch shifter)
            float base = seconds > 0.0f ? 1.0f / (seconds * fps) : 1.0f;
            const float ratio = f.ratio;
            float plan = 0.0f;
            int last = -1;
            float fp = -1.0f;                    // this peak's frequency (computed when needed)
            float lastF = -1.0f, tr = 1.0f, ti = 0.0f;   // the turn of the last frequency used
            int shift = 0;
            const float thr = purityThreshold(purity, S.maxPeak), pw = purityWidth(purity);
            float pg = 1.0f, maxPeak = 0.0f;
            for (int k = 1; k < N / 2; k++)
            {
               int p = f.peak[k];
               float a = base * S.weight[k];
               a = a > 1.0f ? 1.0f : a;
               if (p != last)
               {
                  // a new input peak's bins: the peak learns its frequency if
                  // the input carries it, then runs on one frame
                  last = p;
                  fp = -1.0f;
                  maxPeak = fmaxf(maxPeak, S.mag[p]);
                  pg = purityGain(S.mag[p], thr);
                  if (f.tonalNow[p] > 0.5f && mag[p] >= 0.95f * S.last[p] && mag[p] > 0.7f * S.mag[p])
                  {
                     fp = peakFrequency(p, f.turn[2 * p], f.turn[2 * p + 1]);
                     S.freq[p] = fp;
                  }
                  float fq = S.freq[p];
                  if (fq != lastF)
                  {
                     lastF = fq;
                     quarterTurns((double)fq * ratio, tr, ti);
                     shift = ratio != 1.0f ? (int)lrint(fq * (ratio - 1.0f)) : 0;
                  }
                  float zr = S.z[2 * p], zi = S.z[2 * p + 1];
                  float nr = zr * tr - zi * ti, ni = zr * ti + zi * tr;
                  float fix = 1.5f - 0.5f * (nr * nr + ni * ni);
                  S.z[2 * p] = nr * fix;
                  S.z[2 * p + 1] = ni * fix;
               }
               S.mag[k] += (mag[k] - S.mag[k]) * a;
               plan += S.mag[k] * S.mag[k];
               bool steady = mag[k] >= 0.95f * S.last[k];   // not the frames of a note's end
               S.last[k] = mag[k];
               if (mag[k] > 0.7f * S.mag[k]) S.fresh[k] = 1.0f;   // carried by the input (hysteresis)
               else if (mag[k] < 0.4f * S.mag[k]) S.fresh[k] = 0.0f;
               bool learn = S.fresh[k] > 0.5f && steady;
               // tonal or noisy: the input's test flips per frame on real
               // sounds (vibrato, beating partials); smoothed, with hysteresis
               // (2 frames to become tonal, 3 to stop). (Smeared like the
               // levels, as a phase jitter, a new note came in as noise for the
               // smear time and peaks popped no less)
               if (learn)
               {
                  S.tv[k] += (f.tonalNow[k] - S.tv[k]) * 0.4f;
                  if (S.tv[k] > 0.6f) S.tonal[k] = 1.0f;
                  else if (S.tv[k] < 0.3f) S.tonal[k] = 0.0f;
               }
               float cr, ci;   // the bin's running phase
               int t;
               if (k == p)
               {
                  cr = S.z[2 * k];   // ran on above
                  ci = S.z[2 * k + 1];
                  t = k + shift;
               }
               else if (learn && f.tonalNow[k] > 0.5f && f.tonalNow[p] > 0.5f)
               {
                  // learns: its peak's frequency, and its phase as the peak's
                  // running phase plus its offset to the peak in the input
                  if (fp < 0.0f)
                     fp = S.freq[p];
                  S.freq[k] = fp;
                  float xr = f.spec[2 * k], xi = f.spec[2 * k + 1];
                  float pr = f.spec[2 * p], pi = f.spec[2 * p + 1];
                  float dr = xr * pr + xi * pi, di = xi * pr - xr * pi;   // x * conj(peak)
                  float dm = dr * dr + di * di;
                  if (dm > 1e-30f)
                  {
                     float inv = 1.0f / sqrtf(dm);
                     dr *= inv;
                     di *= inv;
                  }
                  else
                  {
                     dr = 1.0f;
                     di = 0.0f;
                  }
                  float zr = S.z[2 * p], zi = S.z[2 * p + 1];
                  cr = zr * dr - zi * di;
                  ci = zr * di + zi * dr;
                  t = k + (S.freq[p] == lastF ? shift : (ratio != 1.0f ? (int)lrint(fp * (ratio - 1.0f)) : 0));
               }
               else
               {
                  // runs on at its own frequency
                  float fq = S.freq[k];
                  if (fq != lastF)
                  {
                     lastF = fq;
                     quarterTurns((double)fq * ratio, tr, ti);
                     shift = ratio != 1.0f ? (int)lrint(fq * (ratio - 1.0f)) : 0;
                  }
                  float zr = S.z[2 * k], zi = S.z[2 * k + 1];
                  cr = zr * tr - zi * ti;
                  ci = zr * ti + zi * tr;
                  float fix = 1.5f - 0.5f * (cr * cr + ci * ci);
                  cr *= fix;
                  ci *= fix;
                  t = k + shift;
               }
               S.z[2 * k] = cr;
               S.z[2 * k + 1] = ci;
               float nr = cr, ni = ci;
               if (S.tonal[k] < 0.5f)
               {
                  noise(f.rng);   // not a stable partial: a random phase
                  const float *u = unitTable[f.rng >> 24];
                  nr = u[0];
                  ni = u[1];
               }
               if (t < 1 || t >= N / 2)
                  continue;
               float m = S.mag[k] * w * pg * widthGain(k, p, pw) * (((t - k) & 1) ? -1.0f : 1.0f);   // (moved: see freezeLayer)
               f.syn[2 * t] += m * nr;
               f.syn[2 * t + 1] += m * ni;
               if (f.syn2)
               {
                  f.syn2[2 * t] += m * f.pan[t] * nr;
                  f.syn2[2 * t + 1] += m * f.pan[t] * ni;
               }
            }
            S.planned += (plan - S.planned) * perFrame(0.3f);
            S.maxPeak = maxPeak;
         }

         // FREEZE (see nemesis_spectral.h), in three parts run in different
         // audio blocks: freezeStart() (parameters, captures, fades, the
         // smear), freezeLayer() for each layer, freezeEnd() (the inverse
         // FFT, the overlap-add, the corrections)
         FLASHMEM void freezeStart(Channel &s, State &f, const float *mag)
         {
            float a = s.p[0];
            float step = s.p[1] > 1e-5f ? s.p[1] : 1e-5f;
            float drift = clamp(s.p[2], 0, 1), seconds = s.p[3], motion = clamp(s.p[4], 0, 1);
            float tilt = clamp(s.p[6], -1, 1);
            float morph = clamp(s.p[7], 0, 1), hold = clamp(s.p[8], 0, 1), grain = clamp(s.p[9], 0, 1);
            float pitch = clamp(s.p[10], 0.25f, 4.0f), amount = clamp(s.p[11], 0, 1);
            float purity = clamp(s.p[12], 0, 1);
            f.ratio *= powf(pitch / f.ratio, perFrame(0.03f));   // pitch glides (~30 ms, in octaves)
            f.waiting = -1;
            if (a > 0.0f && a != f.captured)
            {
               // into the layer that isn't playing; if it still sounds (held,
               // or fading out) it fades out quickly first (~12 ms; a capture
               // into it at once cut it off)
               int l = f.layer[f.current].g > 0.0f ? 1 - f.current : f.current;
               if (f.layer[l].g <= 0.0f)
                  capture(f, a, l, morph, hold);
               else
                  f.waiting = l;
            }
            if (a <= 0.0f)
               f.captured = 0.0f;   // the next freeze captures anew
            const float quick = fmaxf(step, HOP / (0.012f * rate));
            float moved = 0.0f;   // level a waiting layer hands to the current one
            bool any = false;
            for (int l = 0; l < 2; l++)
            {
               Layer &L = f.layer[l];
               float st = step;
               if (a <= 0.0f)
                  L.target = 0.0f;
               else if (l == f.current)
                  L.target = 1.0f;
               else if (l == f.waiting)
               {
                  L.target = 0.0f;
                  st = quick;
               }
               else
                  L.target = L.filled ? hold : 0.0f;
               if (l == f.waiting && hold <= 0.0f && a > 0.0f)
               {
                  // a capture during a glide: the layer fading out plays the
                  // same partials in phase as the current one (it glides
                  // towards them), so its level moves over to it instead of
                  // dipping for the quick fade
                  float d = fminf(L.g, st);
                  L.g -= d;
                  moved += d;
               }
               else if (L.g < L.target) L.g = fminf(L.target, L.g + st);
               else if (L.g > L.target) L.g = fmaxf(L.target, L.g - st);
               if (L.u < 1.0f) L.u = fminf(1.0f, L.u + step);   // the morph glide, at the fade's speed
               any |= L.g > 0.0f;
            }
            if (moved > 0.0f)
            {
               Layer &C = f.layer[f.current];
               C.g = fminf(1.0f, C.g + moved);
               any = true;
            }
            Smear &S = f.smear;
            // the smear plays while it's on, and with a pitch (no inertia
            // then: the live input transposed)
            bool live = pitch < 0.9999f || pitch > 1.0001f;
            float smearTarget = live ? 1.0f : amount;   // (seconds 0: no inertia, the input resynthesized)
            // (at most 0.2 s: the fade step can be a minute, the smear knob must answer)
            const float sstep = fmaxf(step, HOP / (0.2f * rate));
            if (S.g < smearTarget) S.g = fminf(smearTarget, S.g + sstep);
            else if (S.g > smearTarget) S.g = fmaxf(smearTarget, S.g - sstep);
            if (S.g <= 0.0f)
               S.primed = false;   // starts from the input when it comes back
            float gf = fminf(1.0f, f.layer[0].g + f.layer[1].g);   // the frozen sound's share
            // the smear's, against the frozen sound: both come from the same
            // input, partly in phase: a linear crossfade dipped ~1 dB in the
            // middle (more on uncorrelated sounds), equal power rose 2-3 dB.
            // Both go with x^0.7 (0.62 each in the middle)
            float ws = S.g * powf(1.0f - gf, 0.7f);
            f.frozenShape = gf > 0.0f ? powf(gf, 0.7f) / gf : 1.0f;   // the layers' gain, scaled together
            f.level = fminf(1.0f, gf + ws);
            f.gf = gf;
            f.ws = ws;
            f.active = any || ws > 0.0f;
            if (!f.active)
               return;
            memset(f.syn, 0, N * sizeof(float));
            if (f.syn2)
               memset(f.syn2, 0, N * sizeof(float));
            lap(3);
            if (ws > 0.0f)
               smear(f, mag, seconds > 0.0f ? fmaxf(seconds, 0.05f) : 0.0f, tilt, ws * S.corr, purity);
            lap(4);

            // drift: every peak's pitch wanders by up to +-15 cents (a
            // random target every ~0.34 s, glided to). (Random turns per
            // frame were heard as a buzz: at drift 0.73 the noise around a
            // frozen sine was only 13 dB down.) With the pitch ratio and the
            // morph glide it sets each peak's ratio: its bins move by the
            // nearest whole number of bins, its phase turns at the new
            // frequency (per peak: per bin it undid the phase locking, 3-5 dB
            // lost to cancellation)
            // drift: every partial's pitch up to +-50 cents, its level up to
            // +-4 dB, rising with drift^2 (0.5: +-12 cents). (+-15 cents and
            // +-1.5 dB were hardly heard: 5 cents spread on a frozen piano)
            f.cents = drift * drift * 50.0f;
            f.life = drift * drift * 0.45f;
            f.grain = grain;
            f.purity = purity;
            // grain: a random phase offset per bin on the stable partials,
            // only in the output (no pitch wander): +-grain^2 * pi
            f.spread = grain * grain * 128.0f;   // in table steps (256 per turn)
            // two layers held together: their sum kept at the level of one
            float g0 = f.layer[0].g, g1 = f.layer[1].g;
            float both = sqrtf(g0 * g0 + g1 * g1);
            f.share = both > 1.0f ? 1.0f / both : 1.0f;
            f.scatter = motion;   // (parameter e)
            f.scatterA = perFrame(fmaxf(s.p[5], 0.05f));   // its smoothing, seconds (parameter f)
            f.lock = clamp(s.p[13], 0, 1);
         }

         // one frozen layer into the spectrum to resynthesize (f.syn)
         FLASHMEM void freezeLayer(State &f, int l)
         {
            Layer &L = f.layer[l];
            if (!f.active || L.g <= 0.0f)
               return;
            const float cents = f.cents, spread = f.spread, share = f.share;
            const float life = f.life, jitter = f.grain * 0.6f;
            const uint32_t newTarget = (uint32_t)(65536.0f / (0.34f * fps));
            const uint32_t newLife = (uint32_t)(65536.0f / (0.7f * fps));   // level: a new target every ~0.7 s
            const float glide = perFrame(0.18f), lifeGlide = perFrame(0.35f);
            const float thr = purityThreshold(f.purity, L.maxPeak), pw = purityWidth(f.purity);
            float maxPeak = 0.0f;
            {
               // scatter: every bin plays the level of a frame chosen at random
               // from the newest F of the capture (the newest holds the
               // average), smoothed (each bin drifts on its own: no common
               // movement, no repeating wobble). 0: the averaged hold
               const int newestRow = (L.start + M - 1) % M;
               int F = f.scatter > 0.0f ? 2 + (int)(f.scatter * (M - 2) + 0.5f) : 1;
               F = F > L.valid ? L.valid : F;
               const float sa = f.scatterA;
               // lock: 1 = every bin turns with its peak (clean partials);
               // lower, it moves towards its own frequency and its phase
               // wanders on its own (diffuse, lush)
               const float lock = f.lock, own = 1.0f - lock;
               // how far a bin runs at its own frequency (the chosen frame's):
               // lock below 1, or scatter (the random frame's frequency, as
               // the level; without it scatter only moved levels, smoothed
               // away on sustained sounds)
               const float ownF = fmaxf(own, f.scatter);
               // the phase wander: pulled back to 0 within ~0.5 s, up to
               // +-90 degrees at lock 0. Slow and bounded: across the 4
               // frames of a window the phases hardly change, so the
               // overlap-add keeps its level (an unbounded random walk
               // decorrelated the frames: 3 dB quieter at any lock < 1)
               const float pull = expf(-1.0f / (0.5f * fps));
               // bins that run at their own frequencies no longer add up in
               // phase across the overlapping frames: a partial loses up to
               // 3 dB (measured: at 2048 points 0.2 dB at lock 0.99, 1.2 at
               // 0.9, ~3 from 0.75; at 4096 it rises over the whole range).
               // Made up on the tonal bins at once, so a freeze's crossfade
               // doesn't dip before the output correction catches up
               const float ownComp = ownF > 0.0f ? exp2f(0.5f * (1.0f - expf(-ownF / (0.2f * N / 2048.0f)))) : 1.0f;
               const float kick = own * 1.5708f * sqrtf(3.0f * (1.0f - pull * pull));
               float played = 0.0f;   // energy of the levels played this frame (before the gain)
               int lastPeak = -1;
               float rr = 1.0f, ri = 0.0f;   // this peak's turn this frame
               int shift = 0;                // and how far its bins move
               float amp = 1.0f;             // and its level: life x purity
               double peakRatio = 1.0;       // and its pitch ratio
               float wl = L.g * f.corr * share * f.frozenShape;
               for (int k = 1; k < N / 2; k++)
               {
                  int p = L.peakOf[k];
                  if (p != lastPeak)
                  {
                     lastPeak = p;
                     float oct = bendNow(L, p);
                     if (cents > 0.0f)
                     {
                        float &w = L.wander[p];
                        float &goal = L.aim[p];
                        if ((f.rng & 0xFFFF) < newTarget)
                           goal = noise(f.rng);
                        noise(f.rng);
                        w += (goal - w) * glide;
                        oct += cents * w * (1.0f / 1200.0f);
                     }
                     float ratio = oct != 0.0f ? f.ratio * exp2f(oct) : f.ratio;
                     double fp = L.freq[p];
                     shift = (int)lrint(fp * (ratio - 1.0));
                     quarterTurns(fp * ratio, rr, ri);   // a bin turns pi/2 per bin and hop
                     peakRatio = ratio;
                     amp = 1.0f;
                     if (life > 0.0f)
                     {
                        // each partial's level wanders slowly (with drift):
                        // a sustain that breathes instead of standing still
                        float &w = L.life[p];
                        float &goal = L.lifeAim[p];
                        if ((f.rng & 0xFFFF) < newLife)
                           goal = noise(f.rng);
                        noise(f.rng);
                        w += (goal - w) * lifeGlide;
                        amp = 1.0f + life * w;
                     }
                     float lp = L.level[p];
                     maxPeak = fmaxf(maxPeak, lp);
                     amp *= purityGain(lp, thr);
                  }
                  float zr = L.z[2 * k], zi = L.z[2 * k + 1];
                  float br = rr, bi = ri;
                  // the frame this bin plays (scatter: a random one): its
                  // level and frequency, one read (PSRAM)
                  const float *chosen = L.frames + newestRow * 2 * K + 2 * k;
                  if (F > 1)
                  {
                     noise(f.rng);
                     int r = (int)(((f.rng >> 16) * (uint32_t)F) >> 16);
                     chosen = L.frames + ((newestRow - r + M) % M) * 2 * K + 2 * k;
                  }
                  bool tonal = L.tonal[k] > 0.5f;
                  if (ownF > 0.0f && tonal)
                  {
                     // its own frequency (moved with its peak's ratio): a
                     // turn per hop only tells it within 4 bins, the reading
                     // nearest its partial's is the one (a bin a few bins
                     // from its peak read 4 bins off: it ran at a wrong
                     // pitch and the partial lost 3 dB in the overlap-add)
                     double fq = L.freq[p], fb = chosen[1];
                     fb += 4.0 * rint((fq - fb) * 0.25);
                     quarterTurns((fq + (fb - fq) * ownF) * peakRatio, br, bi);
                  }
                  float nr = zr * br - zi * bi, ni = zr * bi + zi * br;
                  if (!tonal)
                  {
                     // not a stable partial: a random phase (stochastic
                     // resynthesis, from a table of unit vectors)
                     noise(f.rng);
                     const float *u = unitTable[f.rng >> 24];
                     nr = u[0];
                     ni = u[1];
                  }
                  float fix = 1.5f - 0.5f * (nr * nr + ni * ni);   // back to unit length
                  nr *= fix;
                  ni *= fix;
                  L.z[2 * k] = nr;
                  L.z[2 * k + 1] = ni;
                  if (own > 0.0f && tonal)
                  {
                     // and its slow phase wander, in the output only
                     float &w = L.wander2[k];
                     w = w * pull + kick * noise(f.rng);
                     const float *u = unitTable[(int)(w * 40.743665f) & 255];   // 256 / 2 pi
                     float o = nr * u[0] - ni * u[1];
                     ni = nr * u[1] + ni * u[0];
                     nr = o;
                  }
                  float lev = L.level[k] += (chosen[0] - L.level[k]) * sa;
                  played += lev * lev;
                  int t = k + shift;
                  if (t < 1 || t >= N / 2)
                     continue;
                  if (tonal && spread > 0.0f)
                  {
                     const float *u = unitTable[(int)(noise(f.rng) * spread) & 255];
                     float o = nr * u[0] - ni * u[1];
                     ni = nr * u[1] + ni * u[0];
                     nr = o;
                  }
                  // moved by an odd number of bins: the opposite sign (a bin's
                  // phase counts from the window's start, the sound's from
                  // its centre: pi per bin. Without it a gliding partial
                  // flipped its phase at every bin it crossed: a dropout)
                  float m = wl * lev * L.norm * amp * widthGain(k, p, pw) * ((shift & 1) ? -1.0f : 1.0f);
                  if (tonal)
                     m *= ownComp;
                  if (jitter > 0.0f)
                  {
                     // grain: a random level per bin and frame (with the
                     // phase offset above, the component moves within its bin)
                     float j = 1.0f + jitter * noise(f.rng);
                     m *= j > 0.0f ? j : 0.0f;
                  }
                  f.syn[2 * t] += m * nr;
                  f.syn[2 * t + 1] += m * ni;
                  if (f.syn2)
                  {
                     f.syn2[2 * t] += m * f.pan[t] * nr;
                     f.syn2[2 * t + 1] += m * f.pan[t] * ni;
                  }
               }
               // the gain follows the played levels within ~15 ms (slower let
               // fast motion through attacks jump by 7 dB), +-12 dB at most
               if (played > 1e-20f && L.targetE > 0.0f)
               {
                  float want = clamp(sqrtf(L.targetE / played), 0.25f, 4.0f);
                  L.norm += (want - L.norm) * perFrame(0.016f);
               }
               L.maxPeak = maxPeak;
            }
            lap(5);
         }

         FLASHMEM void freezeEnd(Channel &s, State &f)
         {
            s.level = f.level;   // with the sound it describes (slush aligns to it)
            if (!f.active)
               return;
            Smear &S = f.smear;
            const float gf = f.gf;
            inverse(f.syn, f.time);
            for (int i = 0; i < N; i++)
               s.out[(s.pos + i) & MASK] += f.time[i] * window[i] * gain;
            if (s.out2)
               for (int i = 0; i < N; i++)
                  s.out2[(s.pos + i) & MASK] += f.time[i] * window[i] * gain;
            lap(6);

            // output corrections: the overlap-add loses level where the
            // bins of close partials share a turn (a low chord: -2.5 dB),
            // and smoothed levels carry less energy than the input's. The
            // next HOP output samples are complete now: their energy against
            // the input's adjusts the frozen sound's correction (while one
            // layer plays alone) or the smear's (while it plays alone)
            Layer &C = f.layer[f.current];
            bool frozenAlone = C.g >= 1.0f && f.layer[1 - f.current].g <= 0.0f && C.targetT > 1e-12f;
            bool smearAlone = gf <= 0.0f && S.g >= 1.0f && f.energyT > 1e-12f;
            if (frozenAlone || smearAlone)
            {
               float e = 0.0f;
               for (int i = 0; i < HOP; i++)
               {
                  float y = s.out[(s.pos + i) & MASK];
                  e += y * y;
               }
               e /= HOP;
               if (frozenAlone && e > 1e-14f)
               {
                  float r = clamp(sqrtf(C.targetT / e), 0.8f, 1.25f);   // a step at most +-2 dB
                  f.corr *= 1.0f + perFrame(0.05f) * (r - 1.0f);
                  f.corr = clamp(f.corr, 0.5f, 2.0f);
               }
               else if (smearAlone)
               {
                  // the smear: against the energy its levels mean (the input's
                  // ratio of time energy to spectral energy makes that a time
                  // energy), not the input's energy: the smear lags the input
                  // on purpose, and following the input (and the output's
                  // energy of one hop) moved the level +-2 dB a few times a
                  // second (steps)
                  S.played += (e / (S.corr * S.corr) - S.played) * perFrame(0.3f);
                  if (S.played > 1e-14f && f.energy > 1e-14f)
                  {
                     float want = clamp(sqrtf(f.energyT / f.energy * S.planned / S.played), 0.5f, 2.0f);
                     S.corr += (want - S.corr) * perFrame(0.5f);
                  }
               }
            }
            if (s.out2)
            {
               // mono at 4096: the side, left + and right -
               inverse(f.syn2, f.time);
               for (int i = 0; i < N; i++)
               {
                  float v = f.time[i] * window[i] * gain;
                  s.out[(s.pos + i) & MASK] += v;
                  s.out2[(s.pos + i) & MASK] -= v;
               }
            }
            lap(7);
         }
      }

      FLASHMEM void frame(int c)
      {
         Channel &s = channel[c];
         State &f = state[c];
         if (!f.nparts)
            return;
         profMark = now();
         // the last N input samples, oldest first
         for (int i = 0; i < N; i++)
            f.time[i] = s.in[(s.pos + i) & MASK] * window[i];
         forward(f.time, f.spec);
         lap(0);
         if (s.kernel == FREEZE)
         {
            // this frame's levels into the history, its energy, its peaks
            // and locked turns
            float *h = f.mag;
            h[0] = 0.0f;
            h[K - 1] = 0.0f;
            float e = 0.0f;
            for (int k = 1; k < N / 2; k++)
            {
               float re = f.spec[2 * k], im = f.spec[2 * k + 1];
               float m2 = re * re + im * im;
               h[k] = sqrtf(m2);
               e += m2;
            }
            f.energy += (e - f.energy) * perFrame(0.15f);   // (0.68 s: a freeze on a fading note came out ~3 dB louder than the note)
            float et = 0.0f;   // the input's last HOP samples
            for (int i = N - HOP; i < N; i++)
            {
               float x = s.in[(s.pos + i) & MASK];
               et += x * x;
            }
            f.energyT += (et / HOP - f.energyT) * perFrame(0.15f);
            lap(1);
            findPeaks(h, f.peak);
            lap(2);
            lockedTurns(f);
            tonality(f);
            // every bin's own frequency (its turn since the last frame): a
            // capture keeps it for lock < 1
            for (int k = 1; k < N / 2; k++)
            {
               float cr = f.spec[2 * k], ci = f.spec[2 * k + 1], pr = f.prev[2 * k], pi = f.prev[2 * k + 1];
               f.binF[k] = peakFrequency(k, cr * pr + ci * pi, ci * pr - cr * pi);
            }
            // the running average a capture holds: geometric (in log2), so
            // a piano's attacks don't lift the held sustain (an energy
            // average held it louder than the notes sounded)
            const float av = perFrame(0.15f);
            for (int k = 1; k < N / 2; k++)
               f.avg[k] += (log2f(h[k] + 1e-12f) - f.avg[k]) * av;
            // levels and frequencies into the history (PSRAM, in order)
            float *row = f.hist + f.histPos * 2 * K;
            row[0] = row[1] = 0.0f;
            row[2 * (K - 1)] = 0.0f;
            row[2 * (K - 1) + 1] = (float)(K - 1);
            for (int k = 1; k < N / 2; k++)
            {
               row[2 * k] = h[k];
               row[2 * k + 1] = f.binF[k];
            }
            f.histPos = (f.histPos + 1) % M;
            if (f.histValid < M)
               f.histValid++;
            memcpy(f.lastTurn, f.turn, N * sizeof(float));
            lap(3);   // turns (the synthesis adds its start to it)
            f.step = 1;   // the synthesis follows in steps: synthesis()
         }
         memcpy(f.prev, f.spec, N * sizeof(float));   // synthesis reads spec, turn, mag: not prev
      }

      // the synthesis of a frame in four steps, HOP / 8 samples apart (one
      // audio block apart at 4096 points and 128 samples): the start (the
      // smear), layer 0, layer 1, the inverse FFT and the overlap-add. The
      // last ends half a hop after the analysis, where the other channel's
      // frame starts: the channels never use the shared scratch at once. A
      // whole frame in one block peaked at 105 % of a 96 kHz block when
      // freezing (a click); a 4096 frame in two halves at 138 % of a block
      FLASHMEM void synthesis(int c)
      {
         Channel &s = channel[c];
         State &f = state[c];
         if (!f.step || !f.nparts)
            return;
         profMark = now();
         switch (f.step)
         {
         case 1: freezeStart(s, f, f.mag); break;
         case 2: freezeLayer(f, 0); break;
         case 3: freezeLayer(f, 1); break;
         default:
            freezeEnd(s, f);
            profFrames++;
            f.step = 0;
            return;
         }
         f.step++;
      }

      namespace
      {
         // frees the arrays, keeps the region
         FLASHMEM void release()
         {
            for (int c = 0; c < NEMESIS_SPECTRAL_CHANNELS; c++)
            {
               for (int i = 0; i < state[c].nparts; i++)
                  freeMem(state[c].parts[i]);
#ifdef NM_SPECTRAL_CMSIS
               if (state[c].slow) extmem_free(state[c].slow);
#else
               free(state[c].slow);
#endif
               state[c] = State();
               channel[c] = Channel();
            }
            for (int i = 0; i < ncommon; i++)
               freeMem(common[i]);
            ncommon = 0;
            window = sharedTime = sharedSyn = nullptr;
            regionUsed = inDTCM = inRAM2 = 0;
            used = 0;
         }

         FLASHMEM bool build(int n, int size)
         {
            release();
            if (n < 1 || n > NEMESIS_SPECTRAL_CHANNELS || size > MAX)
               return false;
            N = size;
            HOP = N / 4;
            K = N / 2 + 1;
            MASK = N - 1;
            fps = rate / HOP;
            window = (float *)takeCommon(N * sizeof(float));
            sharedTime = (float *)takeCommon(N * sizeof(float));
            sharedSyn = (float *)takeCommon(N * sizeof(float));
            if (!window || !sharedTime || !sharedSyn || !setupFFT(size))
            {
               release();
               return false;
            }
            for (int i = 0; i < N; i++)
               window[i] = 0.5f - 0.5f * cosf(TAU * i / N);
            for (int i = 0; i < 256; i++)
            {
               unitTable[i][0] = cosf(TAU * i / 256);
               unitTable[i][1] = sinf(TAU * i / 256);
            }
            // per channel on chip (takeMem): frame buffers, phases, turns,
            // the smear, the layers; in PSRAM: three level buffers of M
            // frames (the history and the two layers' frames, which a
            // capture swaps; writing the history there costs ~2 % in a
            // frame's block). At 4096 points two channels need ~760 kB, more
            // than RAM1's arena and RAM2 hold: only channel 0 runs (mono, the
            // program gives it both inputs), channel 1 plays its output
            const size_t slow = (size_t)3 * M * 2 * K * sizeof(float);
            const int run = N > 2048 ? 1 : n;
            for (int c = 0; c < run; c++)
            {
               State &f = state[c];
               Channel &s = channel[c];
               bool ok = true;
               auto take = [&](size_t count, size_t size) -> void * {
                  void *m = ok && f.nparts < State::MAX_PARTS ? takeMem(count * size) : nullptr;
                  if (!m)
                  {
                     ok = false;
                     return nullptr;
                  }
                  f.parts[f.nparts++] = m;
                  return m;
               };
               auto takeF = [&](size_t count) { return (float *)take(count, sizeof(float)); };
               // the arrays a frame touches most first (they get the DTCM)
               f.time = sharedTime;
               f.syn = sharedSyn;
               f.spec = takeF(N);
               f.turn = takeF(N);
               f.mag = takeF(K);
               f.tonalNow = takeF(K);
               f.binF = takeF(K);
               f.avg = takeF(K);
               for (int k = 0; k < K; k++)
                  if (f.avg) f.avg[k] = -40.0f;   // (log2 of ~1e-12: silence)
               if (run < n)
               {
                  // mono: one analysis, the bins spread over the stereo field
                  s.out2 = takeF(N);
                  f.syn2 = takeF(N);
                  f.pan = takeF(K);
                  uint32_t r = 0x2545F491u;
                  for (int k = 0; k < K && f.pan; k++)
                     f.pan[k] = 0.9f * noise(r);
               }
               f.peak = (int *)take(K, sizeof(int));
               f.smear.mag = takeF(K);
               f.smear.weight = takeF(K);
               f.smear.z = takeF(N);
               f.smear.freq = takeF(K);
               f.smear.fresh = takeF(K);
               f.smear.last = takeF(K);
               f.smear.tv = takeF(K);
               f.smear.tonal = takeF(K);
               for (int l = 0; l < 2; l++)
               {
                  f.layer[l].z = takeF(N);
                  f.layer[l].freq = takeF(K);
                  f.layer[l].bendA = takeF(K);
                  f.layer[l].bendB = takeF(K);
                  f.layer[l].life = takeF(K);
                  f.layer[l].level = takeF(K);
                  f.layer[l].wander2 = takeF(K);
                  f.layer[l].lifeAim = takeF(K);
                  f.layer[l].wander = takeF(K);
                  f.layer[l].aim = takeF(K);
                  f.layer[l].tonal = takeF(K);
                  f.layer[l].peakOf = (int *)take(K, sizeof(int));
               }
               s.in = takeF(N);
               s.out = takeF(N);
               f.prev = takeF(N);
               f.lastTurn = takeF(N);
#ifdef NM_SPECTRAL_CMSIS
               void *sl = ok ? extmem_malloc(slow) : nullptr;
#else
               void *sl = ok ? malloc(slow) : nullptr;
#endif
               f.slow = sl;
               if (!ok || !sl)
               {
                  release();   // frees this channel's parts too
                  return false;
               }
               memset(sl, 0, slow);
               float *q = (float *)sl;
               f.hist = q; q += M * 2 * K;
               for (int l = 0; l < 2; l++)
               {
                  f.layer[l].frames = q; q += M * 2 * K;
                  f.layer[l].start = 0;
                  f.layer[l].g = f.layer[l].target = 0.0f;
                  f.layer[l].pos = M - 1;
                  f.layer[l].dir = -1.0f;
                  f.layer[l].u = 1.0f;
               }
               f.histPos = 0;
               f.histValid = 0;
               f.waiting = -1;
               f.ratio = 1.0f;
               f.current = 0;
               f.captured = 0.0f;
               f.energy = f.energyT = 0.0f;
               f.corr = 1.0f;
               f.rng = 0x9E3779B9u * (c + 1);
               s.n = N;
               s.mask = MASK;
               s.hop = HOP;
               s.split = HOP / 8;
               s.pos = 0;
               s.count = c * (HOP / 2);   // channel 1 half a hop later: frames in other blocks
            }
            for (int c = run; c < n; c++)
            {
               channel[c].n = N;        // (the size the program asks for: no request)
               channel[c].follow = 0;   // plays channel 0's output
            }
            used = n;
            // the gain of the FFT pair (CMSIS scales its inverse by 1/N, the
            // plain one too: measured, not assumed) and the overlap-add of
            // Hann^2 at N/4 (1.5)
            State &f = state[0];
            for (int i = 0; i < N; i++)
               f.prev[i] = window[i] * sinf(0.37f * i);
            memcpy(f.time, f.prev, N * sizeof(float));
            forward(f.time, f.spec);
            inverse(f.spec, f.syn);
            double xy = 0, xx = 0;
            for (int i = 0; i < N; i++)
            {
               xy += (double)f.syn[i] * f.prev[i];
               xx += (double)f.prev[i] * f.prev[i];
            }
            gain = (float)(1.0 / ((xx > 0 ? xy / xx : 1.0) * 1.5));
            float *const scratch[] = {f.time, f.spec, f.prev, f.syn};
            for (float *a : scratch)
               memset(a, 0, N * sizeof(float));
            requested = 0;
            return true;
         }
      }

      FLASHMEM bool begin(int n, int size)
      {
         refused = 0;
#ifdef NM_SPECTRAL_CMSIS
         if (!region)
         {
            // what the program left of the arena (it allocated before)
            size_t free = nemesis::arenaFree();
            region = free >= 4096 ? (uint8_t *)nemesis::arenaAllocate(free) : nullptr;
            regionSize = region ? free : 0;
         }
#endif
         return build(n, size);
      }

      FLASHMEM void end()
      {
         release();
         region = nullptr;   // the program's arena is emptied after this
         regionSize = 0;
      }

      int channels() { return used; }

      void profile(uint32_t *cycles, uint32_t &frames)
      {
         frames = profFrames;
         for (int i = 0; i < PROFILE_STEPS; i++)
         {
            cycles[i] = frames ? profSum[i] / frames : 0;
            profSum[i] = 0;
         }
         profFrames = 0;
      }

      int running()
      {
         int r = 0;
         for (int c = 0; c < used; c++)
            r += channel[c].follow < 0 && state[c].nparts > 0;
         return r;
      }
      void memoryUse(size_t &dtcm, size_t &ram2)
      {
         dtcm = inDTCM;
         ram2 = inRAM2;
      }
      int size() { return used ? N : 0; }

      void setRate(float sampleRate)
      {
         rate = sampleRate;
         fps = rate / HOP;
      }

      FLASHMEM void service()
      {
         int want = requested;
         if (!used || !want || want == N)
            return;
         int n = used, old = N;
#ifdef NM_SPECTRAL_CMSIS
         NVIC_DISABLE_IRQ(IRQ_SOFTWARE);   // the audio update (AudioNoInterrupts): no frame while the buffers change
#endif
         refused = 0;
         if (!build(n, want))
         {
            // keep the old size if the new one doesn't fit, and don't try
            // it again (every try reset the sound) until another size is
            // asked for or the program restarts
            build(n, old);
            refused = want;
         }
#ifdef NM_SPECTRAL_CMSIS
         NVIC_ENABLE_IRQ(IRQ_SOFTWARE);
#endif
         requested = 0;
      }
   }
}
