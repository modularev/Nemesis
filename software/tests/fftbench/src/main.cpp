// SPDX-License-Identifier: GPL-3.0-or-later
//
// fftbench - the cost of CMSIS-DSP's real FFT on the Teensy 4.1 at 816 MHz,
// for spectral effects (an STFT: window, FFT, work on the bins, inverse
// FFT, window, overlap-add). Prints cycles per call and the CPU share at
// 48 kHz with 4x overlap (a frame every n/4 samples), per channel, with the
// buffers in DTCM, RAM2 (DMAMEM, cached) and PSRAM (EXTMEM, cached).
// Plain sketch (no Nemesis library); results on the serial port at start,
// again on any input line.
#include <Arduino.h>
#include <arm_math.h>

extern "C" uint8_t external_psram_size;   // MB, set by the startup code

constexpr int MAXN = 4096;
constexpr float CLOCK = 816e6f;
constexpr float SR = 48000.0f;

alignas(32) float dtcmA[MAXN], dtcmB[MAXN], dtcmW[MAXN], dtcmO[MAXN];
alignas(32) DMAMEM float ram2A[MAXN], ram2B[MAXN], ram2W[MAXN], ram2O[MAXN];
alignas(32) EXTMEM float psA[MAXN], psB[MAXN], psW[MAXN], psO[MAXN];

struct Buffers
{
   const char *name;
   float *a, *b, *w, *o;   // time frame, spectrum, window, overlap-add output
};

static uint32_t rng = 1;
static float rnd() { rng = rng * 1664525u + 1013904223u; return (int32_t)rng * (1.0f / 2147483648.0f); }

// cycles of f (prepare runs before each call, not counted)
template <typename P, typename F>
static uint32_t cycles(int runs, P prepare, F f)
{
   prepare();
   f();   // warm the caches
   uint32_t best = 0xFFFFFFFF;
   for (int r = 0; r < runs; r++)
   {
      prepare();
      uint32_t t0 = ARM_DWT_CYCCNT;
      f();
      uint32_t t = ARM_DWT_CYCCNT - t0;
      if (t < best) best = t;
   }
   return best;   // the fastest run: no interrupts in it
}
static void none() {}

static void bench(const Buffers &m)
{
   Serial.printf("\n== buffers in %s\n", m.name);
   Serial.printf("   n   fwd   inv   window  polar(n/2)  frame total  CPU/ch @48k 4x\n");
   for (int n = 256; n <= MAXN; n *= 2)
   {
      arm_rfft_fast_instance_f32 S;
      arm_rfft_fast_init_f32(&S, n);
      for (int i = 0; i < n; i++)
      {
         m.a[i] = rnd();
         m.w[i] = 0.5f - 0.5f * cosf(2.0f * PI * i / n);
      }
      // the FFT uses its input as scratch: a fresh copy before each call
      uint32_t fwd = cycles(20, [&] { memcpy(m.o, m.a, n * sizeof(float)); },
                            [&] { arm_rfft_fast_f32(&S, m.o, m.b, 0); });
      uint32_t inv = cycles(20, [&] { memcpy(m.o, m.b, n * sizeof(float)); },
                            [&] { arm_rfft_fast_f32(&S, m.o, m.a, 1); });
      // window the input frame, and window + overlap-add the output frame
      uint32_t win = cycles(20, none, [&] {
         arm_mult_f32(m.a, m.w, m.o, n);
         for (int i = 0; i < n; i++) m.o[i] += m.a[i] * m.w[i];
      });
      // a phase vocoder's per bin work: magnitude and phase, then back
      uint32_t polar = cycles(10, none, [&] {
         for (int k = 1; k < n / 2; k++)
         {
            float re = m.b[2 * k], im = m.b[2 * k + 1];
            float mag = sqrtf(re * re + im * im), ph = atan2f(im, re);
            ph += 0.1f;
            m.b[2 * k] = mag * cosf(ph);
            m.b[2 * k + 1] = mag * sinf(ph);
         }
      });
      uint32_t total = fwd + inv + win + polar;
      float frames = SR / (n / 4);
      Serial.printf("%5d %6lu %6lu %7lu %10lu %12lu  %6.2f %%\n", n, (unsigned long)fwd, (unsigned long)inv,
                    (unsigned long)win, (unsigned long)polar, (unsigned long)total, 100.0f * frames * total / CLOCK);
   }
}

static void run()
{
   Serial.printf("fftbench: CMSIS-DSP arm_rfft_fast_f32, %lu MHz, cycles (best of 20)\n", F_CPU_ACTUAL / 1000000);
   Serial.printf("frame total = fwd + inv + window/overlap-add + polar; CPU for one channel, hop n/4 at 48 kHz\n");
   bench({"DTCM", dtcmA, dtcmB, dtcmW, dtcmO});
   bench({"RAM2 (DMAMEM)", ram2A, ram2B, ram2W, ram2O});
   if (external_psram_size)
      bench({"PSRAM (EXTMEM)", psA, psB, psW, psO});
   else
      Serial.println("\n(no PSRAM)");
   Serial.println("done");
}

void setup()
{
   Serial.begin(115200);
   while (!Serial && millis() < 4000) {}
   delay(500);
   run();
}

void loop()
{
   if (Serial.available())
   {
      while (Serial.available()) Serial.read();
      run();
   }
}
