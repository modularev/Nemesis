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
#else
#define FLASHMEM
#endif

namespace nemesis
{
   namespace spectral
   {
      Channel channel[NEMESIS_SPECTRAL_CHANNELS];
      volatile int requested = 0;
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
         // stay together instead of drifting apart into a metallic smear)
         struct Layer
         {
            float *frames = nullptr;   // M * K levels: the history's ring as it was at the capture (PSRAM)
            int start = 0;             // its oldest frame
            float *z = nullptr;        // N: phase per bin as a unit vector, packed as the spectrum
            float *rot = nullptr;      // N: phase turn per hop
            int *peakOf = nullptr;     // K: each bin's peak (drift turns a peak's bins together)
            float *tonal = nullptr;    // K: 1 = a stable partial (locked phase), 0 = random phase each frame
            float *wander = nullptr;   // K: drift per peak (at its bin), -1..1, gliding to
            float *aim = nullptr;      // K: its random target (a new one every ~0.34 s)
            float g = 0, target = 0;   // fade
            float pos = M - 1, dir = -1;   // place in the frames (motion)
            float norm = 1;            // level match: gain towards the input's energy at the capture
            float targetE = 0;         // that energy (per frame)
            float targetT = 0;         // the input's energy per sample at the capture
         };

         // the smear: per bin a level that follows the input with inertia;
         // the phases turn with the input's current turn where a stable
         // partial is (locked to its peak), else get a random phase. (Gliding
         // the turns as well left the frames out of step with the waveform
         // inside them: a buzz, sidebands at the frame rate 34 dB down.)
         struct Smear
         {
            float *mag = nullptr;      // K
            float *z = nullptr;        // N
            float *weight = nullptr;   // K: tilt per bin (follow speed relative to the middle)
            float tilt = 2.0f;         // the tilt weight was made for (2: none yet)
            float g = 0;               // fade (smear on/off, d > 0)
            bool primed = false;       // state taken from the input
            float corr = 1;            // output level correction (smoothed levels lose energy)
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
            float *mag = nullptr;    // K: this frame's levels (RAM2; copied into the PSRAM history:
                                     // read from there, the peak search took 115k cycles a frame)
            float *hist = nullptr;   // M * K: the levels of the last M frames, a ring (PSRAM)
            int histPos = 0;         // the ring's next frame
            int *peak = nullptr;     // K (scratch): each bin's nearest peak
            Layer layer[2];
            Smear smear;
            int current = 0;
            float captured = 0;      // the a value of the latest capture
            uint32_t rng = 1;
            float energy = 0;        // the input's energy per frame, averaged over ~0.7 s
            float energyT = 0;       // the same per input sample (time domain)
            float corr = 1;          // freeze output level correction, kept from capture to capture
            void *block = nullptr;   // RAM2
            void *slow = nullptr;    // PSRAM
         };

         State state[NEMESIS_SPECTRAL_CHANNELS];
         float window[MAX];   // periodic Hann: Hann^2 at a hop of N/4 sums to 1.5
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
         // size, and the linker kept ~70 kB of them in RAM1
         bool setupFFT(int n)
         {
            if (n == 1024)
            {
               fft.Sint = arm_cfft_sR_f32_len512;
               fft.pTwiddleRFFT = (float32_t *)twiddleCoef_rfft_1024;
            }
            else if (n == 2048)
            {
               fft.Sint = arm_cfft_sR_f32_len1024;
               fft.pTwiddleRFFT = (float32_t *)twiddleCoef_rfft_2048;
            }
            else
               return false;
            fft.fftLenRFFT = n;
            return true;
         }
         // both use their input as scratch
         void forward(float *time, float *spec) { arm_rfft_fast_f32(&fft, time, spec, 0); }
         void inverse(float *spec, float *time) { arm_rfft_fast_f32(&fft, spec, time, 1); }
#else
         // a plain complex FFT for the tests on the Mac, with CMSIS's packing
         float re[MAX], im[MAX];
         bool setupFFT(int n) { return n == 1024 || n == 2048; }
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

         // the history into a layer (the other one if the current still
         // sounds: the two crossfade), with this frame's phases and turns
         FLASHMEM void capture(State &f, float a)
         {
            int l = f.layer[f.current].g > 0.0f ? 1 - f.current : f.current;
            Layer &L = f.layer[l];
            // the history becomes the layer's frames, the history goes on in
            // the layer's old buffer: no copy (copying 66 kB into PSRAM took
            // longer than an audio block: a late block, a click at every
            // freeze). Frames older than the last capture in that buffer are
            // stale until the history has filled it again.
            float *b = L.frames;
            L.frames = f.hist;
            f.hist = b;
            L.start = f.histPos;   // the oldest frame of the ring
            for (int k = 1; k < N / 2; k++)
            {
               float cr = f.spec[2 * k], ci = f.spec[2 * k + 1];
               float m = sqrtf(cr * cr + ci * ci);
               if (m > 1e-12f) { L.z[2 * k] = cr / m; L.z[2 * k + 1] = ci / m; }
               else { L.z[2 * k] = 1.0f; L.z[2 * k + 1] = 0.0f; }
               L.peakOf[k] = f.peak[k];
               L.rot[2 * k] = f.turn[2 * k];
               L.rot[2 * k + 1] = f.turn[2 * k + 1];
               L.tonal[k] = f.tonalNow[k];
            }
            // level match: a capture right after an attack holds a loud
            // stretch, one late in a decaying note a quiet one (+-11 dB
            // with a struck chord): the played levels are steered to the
            // input's recent average energy (freeze() follows it every
            // frame), starting at the exact gain for the newest frame
            const float *mag = L.frames + ((L.start + M - 1) % M) * K;
            L.targetE = f.energy;
            L.targetT = f.energyT;
            double e0 = 0;
            for (int k = 1; k < N / 2; k++)
               e0 += (double)mag[k] * mag[k];
            float r = e0 > 1e-20 ? sqrtf((float)(L.targetE / e0)) : 1.0f;
            L.norm = clamp(r, 0.25f, 4.0f);
            L.g = 0.0f;
            L.pos = M - 1;
            L.dir = -1.0f;
            f.current = l;
            f.captured = a;
         }

         // the smear: levels and turns follow the input (with this frame's
         // levels in mag), phases move by the smoothed turns
         FLASHMEM void smear(State &f, const float *mag, float seconds, float tilt, float w)
         {
            Smear &S = f.smear;
            if (!S.primed)
            {
               // start from the input as it is
               for (int k = 1; k < N / 2; k++)
               {
                  S.mag[k] = mag[k];
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
            float base = 1.0f / (seconds * fps);   // per frame, at the middle
            // the peaks of tonal partials carry the phase on, turning by
            // their measured turn
            for (int k = 1; k < N / 2; k++)
            {
               if (f.peak[k] != k || f.tonalNow[k] < 0.5f)
                  continue;
               float rr = f.turn[2 * k], ri = f.turn[2 * k + 1];
               float zr = S.z[2 * k], zi = S.z[2 * k + 1];
               float nr = zr * rr - zi * ri, ni = zr * ri + zi * rr;
               float fix = 1.5f - 0.5f * (nr * nr + ni * ni);
               S.z[2 * k] = nr * fix;
               S.z[2 * k + 1] = ni * fix;
            }
            for (int k = 1; k < N / 2; k++)
            {
               float a = base * S.weight[k];
               a = a > 1.0f ? 1.0f : a;
               S.mag[k] += (mag[k] - S.mag[k]) * a;
               float nr, ni;
               int p = f.peak[k];
               if (f.tonalNow[k] > 0.5f)
               {
                  // identity phase locking per frame (Laroche & Dolson): the
                  // peak's phase plus this bin's phase offset to the peak in
                  // the input now, so a partial keeps its shape (set once at
                  // the start, the offsets came from a half empty window: a
                  // smeared sine lost half its level and had sidebands at
                  // the frame rate)
                  float zr = S.z[2 * p], zi = S.z[2 * p + 1];
                  if (p == k)
                  {
                     nr = zr;
                     ni = zi;
                  }
                  else
                  {
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
                     nr = zr * dr - zi * di;
                     ni = zr * di + zi * dr;
                  }
               }
               else
               {
                  noise(f.rng);   // old or noisy energy: a random phase
                  const float *u = unitTable[f.rng >> 24];
                  nr = u[0];
                  ni = u[1];
                  if (p == k)
                  {
                     S.z[2 * k] = nr;   // a peak that becomes tonal starts from here
                     S.z[2 * k + 1] = ni;
                  }
               }
               float m = S.mag[k] * w;
               f.syn[2 * k] += m * nr;
               f.syn[2 * k + 1] += m * ni;
            }
         }

         // FREEZE (see nemesis_spectral.h)
         FLASHMEM void freeze(Channel &s, State &f, const float *mag)
         {
            float a = s.p[0];
            float step = s.p[1] > 1e-5f ? s.p[1] : 1e-5f;
            float drift = clamp(s.p[2], 0, 1), seconds = s.p[3], motion = clamp(s.p[4], 0, 1);
            float speed = clamp(s.p[5], 0, 4), tilt = clamp(s.p[6], -1, 1);
            if (a > 0.0f && a != f.captured)
               capture(f, a);
            if (a <= 0.0f)
               f.captured = 0.0f;   // the next freeze captures anew
            bool any = false;
            for (int l = 0; l < 2; l++)
            {
               Layer &L = f.layer[l];
               L.target = (a > 0.0f && l == f.current) ? 1.0f : 0.0f;
               if (L.g < L.target) L.g = fminf(L.target, L.g + step);
               else if (L.g > L.target) L.g = fmaxf(L.target, L.g - step);
               any |= L.g > 0.0f;
            }
            Smear &S = f.smear;
            float smearTarget = seconds > 0.0f ? 1.0f : 0.0f;
            if (S.g < smearTarget) S.g = fminf(smearTarget, S.g + step);
            else if (S.g > smearTarget) S.g = fmaxf(smearTarget, S.g - step);
            if (S.g <= 0.0f)
               S.primed = false;   // starts from the input when it comes back
            float gf = fminf(1.0f, f.layer[0].g + f.layer[1].g);   // the frozen sound's share
            float ws = S.g * (1.0f - gf);                             // the smear's
            s.level = fminf(1.0f, gf + ws);
            if (!any && ws <= 0.0f)
               return;
            memset(f.syn, 0, N * sizeof(float));
            lap(3);
            if (ws > 0.0f)
               smear(f, mag, seconds > 0.05f ? seconds : 0.05f, tilt, ws * S.corr);
            lap(4);

            // drift: every peak's pitch wanders by up to +-15 cents (a
            // random target every ~0.34 s, glided to); the deviation turns
            // its phase a little further each frame. (Random turns per frame
            // were heard as a buzz: at drift 0.73 the noise around a frozen
            // sine was only 13 dB down.)
            const float cents = drift * 15.0f;
            const float PER_CENT = 0.000577623f;   // ln 2 / 1200: 2^(c/1200) - 1 ~ c * PER_CENT
            const uint32_t newTarget = (uint32_t)(65536.0f / (0.34f * fps));
            const float glide = perFrame(0.18f);
            float lo = (M - 1) - motion * (M - 1);   // motion: back and forth over the newest frames
            for (int l = 0; l < 2; l++)
            {
               Layer &L = f.layer[l];
               if (L.g <= 0.0f)
                  continue;
               // the place in the captured frames: a triangle between lo and the newest
               if (lo >= M - 1)
                  L.pos = M - 1;
               else
               {
                  L.pos += L.dir * speed;
                  if (L.pos < lo) { L.pos = lo + (lo - L.pos); L.dir = 1.0f; }
                  if (L.pos > M - 1) { L.pos = (M - 1) - (L.pos - (M - 1)); L.dir = -1.0f; }
                  if (L.pos < lo) L.pos = lo;
               }
               int i0 = (int)L.pos;
               int i1 = i0 < M - 1 ? i0 + 1 : i0;
               float fr = L.pos - i0;
               const float *m0 = L.frames + ((L.start + i0) % M) * K, *m1 = L.frames + ((L.start + i1) % M) * K;
               float played = 0.0f;   // energy of the levels played this frame (before the gain)
               int lastPeak = -1;
               float dc = 1.0f, ds = 0.0f;   // this peak's extra turn this frame
               float wl = L.g * f.corr;
               for (int k = 1; k < N / 2; k++)
               {
                  float zr = L.z[2 * k], zi = L.z[2 * k + 1];
                  float rr = L.rot[2 * k], ri = L.rot[2 * k + 1];
                  float nr = zr * rr - zi * ri, ni = zr * ri + zi * rr;
                  if (L.tonal[k] < 0.5f)
                  {
                     // not a stable partial: a random phase (stochastic
                     // resynthesis, from a table of unit vectors)
                     noise(f.rng);
                     const float *u = unitTable[f.rng >> 24];
                     nr = u[0];
                     ni = u[1];
                  }
                  else if (cents > 0.0f)
                  {
                     // the same for all bins of a peak (per bin it undid the
                     // phase locking: 3-5 dB lost to cancellation)
                     if (L.peakOf[k] != lastPeak)
                     {
                        lastPeak = L.peakOf[k];
                        float &w = L.wander[lastPeak];
                        float &goal = L.aim[lastPeak];
                        if ((f.rng & 0xFFFF) < newTarget)
                           goal = noise(f.rng);
                        noise(f.rng);
                        w += (goal - w) * glide;
                        // a bin's phase turns pi/2 * k per hop of N/4: a pitch
                        // ratio r adds pi/2 * k * (r - 1)
                        float extra = 1.5707963f * lastPeak * cents * w * PER_CENT;
                        ds = sinf(extra);
                        dc = cosf(extra);
                     }
                     float t = nr * dc - ni * ds;
                     ni = nr * ds + ni * dc;
                     nr = t;
                  }
                  float fix = 1.5f - 0.5f * (nr * nr + ni * ni);   // back to unit length
                  nr *= fix;
                  ni *= fix;
                  L.z[2 * k] = nr;
                  L.z[2 * k + 1] = ni;
                  float lev = m0[k] + (m1[k] - m0[k]) * fr;
                  played += lev * lev;
                  float m = lev * L.norm;
                  f.syn[2 * k] += wl * m * nr;
                  f.syn[2 * k + 1] += wl * m * ni;
               }
               // the gain follows the played levels within ~15 ms (slower let
               // fast motion through attacks jump by 7 dB), +-12 dB at most
               if (played > 1e-20f && L.targetE > 0.0f)
               {
                  float want = clamp(sqrtf(L.targetE / played), 0.25f, 4.0f);
                  L.norm += (want - L.norm) * perFrame(0.016f);
               }
            }
            lap(5);
            inverse(f.syn, f.time);
            for (int i = 0; i < N; i++)
               s.out[(s.pos + i) & MASK] += f.time[i] * window[i] * gain;
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
               if (e > 1e-14f)
               {
                  float r = clamp(sqrtf((frozenAlone ? C.targetT : f.energyT) / e), 0.8f, 1.25f);   // a step at most +-2 dB
                  float &corr = frozenAlone ? f.corr : S.corr;
                  corr *= 1.0f + perFrame(0.05f) * (r - 1.0f);
                  corr = clamp(corr, 0.5f, 2.0f);
               }
            }
            lap(7);
         }
      }

      FLASHMEM void frame(int c)
      {
         Channel &s = channel[c];
         State &f = state[c];
         if (!f.block)
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
            memcpy(f.hist + f.histPos * K, h, K * sizeof(float));   // into the history (PSRAM) in one go
            f.histPos = (f.histPos + 1) % M;
            f.energy += (e - f.energy) * perFrame(0.68f);
            float et = 0.0f;   // the input's last HOP samples
            for (int i = N - HOP; i < N; i++)
            {
               float x = s.in[(s.pos + i) & MASK];
               et += x * x;
            }
            f.energyT += (et / HOP - f.energyT) * perFrame(0.68f);
            lap(1);
            findPeaks(h, f.peak);
            lap(2);
            lockedTurns(f);
            tonality(f);
            freeze(s, f, h);
            profFrames++;
            memcpy(f.lastTurn, f.turn, N * sizeof(float));
         }
         memcpy(f.prev, f.spec, N * sizeof(float));
      }

      FLASHMEM bool begin(int n, int size)
      {
         end();
         if (n < 1 || n > NEMESIS_SPECTRAL_CHANNELS || !setupFFT(size))
            return false;
         N = size;
         HOP = N / 4;
         K = N / 2 + 1;
         MASK = N - 1;
         fps = rate / HOP;
         for (int i = 0; i < N; i++)
            window[i] = 0.5f - 0.5f * cosf(TAU * i / N);
         for (int i = 0; i < 256; i++)
         {
            unitTable[i][0] = cosf(TAU * i / 256);
            unitTable[i][1] = sinf(TAU * i / 256);
         }
         // per channel in RAM2: frame buffers, phases, turns, the smear;
         // in PSRAM: three level buffers of M frames (the history and the
         // two layers' frames, which a capture swaps; writing the history
         // there costs ~2 % in a frame's block)
         const size_t fastFloats = 8 * N + 2 * K + 2 * (2 * N + 3 * K) + (N + 2 * K);
         const size_t fastInts = 3 * K;
         const size_t fast = fastFloats * sizeof(float) + fastInts * sizeof(int);
         const size_t slow = (size_t)3 * M * K * sizeof(float);
         for (int c = 0; c < n; c++)
         {
            void *b = malloc(fast);
#ifdef NM_SPECTRAL_CMSIS
            if (!b)
               b = extmem_malloc(fast);
            void *sl = extmem_malloc(slow);
#else
            void *sl = malloc(slow);
#endif
            if (!b || !sl)
            {
#ifdef NM_SPECTRAL_CMSIS
               if (b) extmem_free(b);
               if (sl) extmem_free(sl);
#else
               free(b);
               free(sl);
#endif
               end();
               return false;
            }
            memset(b, 0, fast);
            memset(sl, 0, slow);
            float *p = (float *)b;
            State &f = state[c];
            Channel &s = channel[c];
            f.block = b;
            f.slow = sl;
            s.in = p; p += N;
            s.out = p; p += N;
            f.time = p; p += N;
            f.spec = p; p += N;
            f.prev = p; p += N;
            f.syn = p; p += N;
            f.turn = p; p += N;
            f.lastTurn = p; p += N;
            f.mag = p; p += K;
            f.tonalNow = p; p += K;
            for (int l = 0; l < 2; l++)
            {
               f.layer[l].z = p; p += N;
               f.layer[l].rot = p; p += N;
               f.layer[l].wander = p; p += K;
               f.layer[l].aim = p; p += K;
               f.layer[l].tonal = p; p += K;
            }
            f.smear.mag = p; p += K;
            f.smear.weight = p; p += K;
            f.smear.z = p; p += N;
            int *ip = (int *)p;
            f.peak = ip; ip += K;
            f.layer[0].peakOf = ip; ip += K;
            f.layer[1].peakOf = ip;
            float *q = (float *)sl;
            f.hist = q; q += M * K;
            for (int l = 0; l < 2; l++)
            {
               f.layer[l].frames = q; q += M * K;
               f.layer[l].start = 0;
               f.layer[l].g = f.layer[l].target = 0.0f;
               f.layer[l].pos = M - 1;
               f.layer[l].dir = -1.0f;
            }
            f.histPos = 0;
            f.current = 0;
            f.captured = 0.0f;
            f.energy = f.energyT = 0.0f;
            f.corr = 1.0f;
            f.rng = 0x9E3779B9u * (c + 1);
            s.n = N;
            s.mask = MASK;
            s.hop = HOP;
            s.pos = 0;
            s.count = c * (HOP / 2);   // channel 1 half a hop later: frames in other blocks
            used = c + 1;
         }
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
         memset(f.time, 0, 4 * N * sizeof(float));   // time, spec, prev, syn
         requested = 0;
         return true;
      }

      FLASHMEM void end()
      {
         for (int c = 0; c < NEMESIS_SPECTRAL_CHANNELS; c++)
         {
#ifdef NM_SPECTRAL_CMSIS
            if (state[c].block) extmem_free(state[c].block);   // frees heap blocks too
            if (state[c].slow) extmem_free(state[c].slow);
#else
            free(state[c].block);
            free(state[c].slow);
#endif
            state[c] = State();
            channel[c] = Channel();
         }
         used = 0;
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

      char placement(int c)
      {
         uintptr_t a = (uintptr_t)state[c].block;
         return !a ? '-' : (a >= 0x70000000u ? 'P' : 'R');
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
         bool ok = begin(n, want);
         if (!ok)
            ok = begin(n, old);   // keep the old size if the new one doesn't fit
#ifdef NM_SPECTRAL_CMSIS
         NVIC_ENABLE_IRQ(IRQ_SOFTWARE);
#endif
         requested = 0;
         (void)ok;
      }
   }
}
