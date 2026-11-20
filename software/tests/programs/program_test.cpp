// SPDX-License-Identifier: GPL-3.0-or-later
//
// program_test.cpp - renders one Nemesis program on the Mac and checks its
// outputs (run.sh builds it once per program from software/programs).
//
// The program is compiled by FAUST as class Prog (juno: voices through
// nemesis-poly.h plus its effect as ProgEffect). The test plays it as the
// module would: the transport sliders ([transport:...]) run at 120 BPM,
// [mono:note/gate/trig/velocity] get notes and triggers, a polyphonic
// program gets chords, and every input gets a signal (IN 1 noise bursts and
// a tone, IN 2 a tone, the others trigger pulses; a program whose inputs
// are all triggers says so with 'declare test_inputs "trig";'). Phases:
//
//   defaults   4 s, the program as it starts (transport stopped for 0.5 s)
//   random     24 x 1 s, every setting random (seeded: repeatable)
//   min / max  2 s each, every setting at its minimum / maximum
//   recovery   4 s, defaults again with silent inputs and no notes
//
// Failures (exit code 1): NaN or infinite values on an output or a meter,
// an output beyond +-100 (runaway feedback), an audio output beyond +-4
// with the defaults. Warnings: beyond +-4 with other settings, silent audio
// outputs with the defaults, DC offset, clipping, a tail that doesn't settle.
// For a failing phase the test looks for the culprit: every setting alone
// at its failing value (the others at their defaults, the program's state
// cleared), 2 s each; if none fails alone, every setting put back to its
// default while the others stay (the settings the failure needs).
//
// usage: program_test <name> [seed]

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
#include <string>
#include <vector>

#define FAUSTFLOAT float
#include "faust/dsp/dsp.h"
#include "faust/gui/UI.h"
#include "faust/gui/meta.h"

// pollen's 16 bit memory (nemesis_mem16.h): plain buffers on the Mac
#include "nemesis_mem16.h"
namespace nemesis
{
   namespace mem16
   {
      int16_t *buffer[NEMESIS_MEM16_CHANNELS];
      bool begin(int) { return true; }
      void end() {}
      int channels() { return NEMESIS_MEM16_CHANNELS; }
   }
}

#include PROG_H
#ifdef PROG_VOICES
#include PROG_EFFECT_H
#include "faust/dsp/nemesis-poly.h"
#endif

namespace
{
   const float RUNAWAY = 100.0f;  // beyond any output level: feedback ran away
   const float LOUD = 4.0f;       // 12 dB above full scale: the output stage clips it hard

   struct Param
   {
      std::string label;
      FAUSTFLOAT *zone;
      float init, min, max, step;
      bool button = false;
      std::map<std::string, std::string> meta;
      bool has(const char *k) const { return meta.count(k) > 0; }
      std::string get(const char *k) const
      {
         auto it = meta.find(k);
         return it == meta.end() ? "" : it->second;
      }
   };

   // every control and meter of the program, with its metadata
   struct Collector : public UI
   {
      std::vector<Param> params;
      std::vector<std::pair<std::string, FAUSTFLOAT *>> meters;
      std::map<FAUSTFLOAT *, std::map<std::string, std::string>> pending;

      void add(const char *l, FAUSTFLOAT *z, float init, float min, float max, float step, bool button)
      {
         Param p{l, z, init, min, max, step};
         p.button = button;
         p.meta = pending[z];
         params.push_back(p);
      }
      void openTabBox(const char *) override {}
      void openHorizontalBox(const char *) override {}
      void openVerticalBox(const char *) override {}
      void closeBox() override {}
      void addButton(const char *l, FAUSTFLOAT *z) override { add(l, z, 0, 0, 1, 1, true); }
      void addCheckButton(const char *l, FAUSTFLOAT *z) override { add(l, z, 0, 0, 1, 1, false); }
      void addVerticalSlider(const char *l, FAUSTFLOAT *z, FAUSTFLOAT i, FAUSTFLOAT a, FAUSTFLOAT b, FAUSTFLOAT s) override { add(l, z, i, a, b, s, false); }
      void addHorizontalSlider(const char *l, FAUSTFLOAT *z, FAUSTFLOAT i, FAUSTFLOAT a, FAUSTFLOAT b, FAUSTFLOAT s) override { add(l, z, i, a, b, s, false); }
      void addNumEntry(const char *l, FAUSTFLOAT *z, FAUSTFLOAT i, FAUSTFLOAT a, FAUSTFLOAT b, FAUSTFLOAT s) override { add(l, z, i, a, b, s, false); }
      void addHorizontalBargraph(const char *l, FAUSTFLOAT *z, FAUSTFLOAT, FAUSTFLOAT) override { meters.push_back({l, z}); }
      void addVerticalBargraph(const char *l, FAUSTFLOAT *z, FAUSTFLOAT, FAUSTFLOAT) override { meters.push_back({l, z}); }
      void addSoundfile(const char *, const char *, Soundfile **) override {}
      void declare(FAUSTFLOAT *z, const char *k, const char *v) override
      {
         if (z)
            pending[z][k] = v;
      }
   };

   struct Declares : public Meta
   {
      std::map<std::string, std::string> values;
      void declare(const char *k, const char *v) override { values[k] = v; }
   };

   struct Stats
   {
      double sum = 0, sum2 = 0, peak = 0;
      long n = 0, bad = 0, clipped = 0;
      void add(float x)
      {
         if (!std::isfinite(x))
         {
            bad++;
            return;
         }
         double a = fabs(x);
         sum += x;
         sum2 += (double)x * x;
         if (a > peak)
            peak = a;
         if (a > 1.0)
            clipped++;
         n++;
      }
      double rms() const { return n ? sqrt(sum2 / n) : 0; }
      double dc() const { return n ? sum / n : 0; }
   };
}

int main(int argc, char **argv)
{
   const char *name = argc > 1 ? argv[1] : "program";
   unsigned seed = argc > 2 ? (unsigned)atoi(argv[2]) : 1;
   for (int c = 0; c < NEMESIS_MEM16_CHANNELS; c++)
      nemesis::mem16::buffer[c] = (int16_t *)calloc(NEMESIS_MEM16_LENGTH, sizeof(int16_t));

   // the program's declares: sample rate, block size, audio outputs
   Declares decl;
   {
      Prog probe;
      probe.metadata(&decl);
   }
   int SR = decl.values.count("sample_rate") ? atoi(decl.values["sample_rate"].c_str()) : 48000;
   // INPUTS=audio: the test signals even on trigger inputs (audio-rate triggering)
   const char *inputsEnv = getenv("INPUTS");
   bool allTrig = decl.values["test_inputs"] == "trig" && !(inputsEnv && strcmp(inputsEnv, "audio") == 0);
   int B = decl.values.count("block_size") ? atoi(decl.values["block_size"].c_str()) : 64;
   if (B <= 0 || B > 128)
      B = 64;

   dsp *d;
#ifdef PROG_VOICES
   const int voices = SR > 48000 ? (PROG_VOICES + 1) / 2 : PROG_VOICES;   // the library halves them at 96 kHz
   std::vector<dsp *> vs;
   for (int i = 0; i < voices; i++)
      vs.push_back(new Prog());
   auto *poly = new nemesis_poly(vs.data(), voices, [](dsp *x) { delete x; }, new ProgEffect(),
                                 [](dsp *x) { delete x; }, B);
   d = poly;
   for (int i = 0; i < voices; i++)
      vs[i]->init(SR);   // classInit: the voices' tables
   ProgEffect::classInit(SR);
#else
   d = new Prog();
#endif
   d->init(SR);
   Collector ui;
   d->buildUserInterface(&ui);

   int nin = d->getNumInputs(), nout = d->getNumOutputs();
   // audio outputs as the library takes them (nemesis_output.h): declared, else 1 and 2
   std::vector<bool> audio(nout, false);
   for (int c = 0; c < nout && c < 2; c++)
      audio[c] = decl.values.count("audio_outputs") == 0;
   if (decl.values.count("audio_outputs"))
   {
      std::string s = decl.values["audio_outputs"];
      // "1,2" or "1-8"
      for (size_t p = 0; p < s.size();)
      {
         size_t q = s.find(',', p);
         std::string item = s.substr(p, q == std::string::npos ? std::string::npos : q - p);
         int a = atoi(item.c_str()), b = a;
         size_t dash = item.find('-');
         if (dash != std::string::npos) b = atoi(item.c_str() + dash + 1);
         for (int o = a; o <= b; o++)
            if (o >= 1 && o <= nout) audio[o - 1] = true;
         p = q == std::string::npos ? s.size() : q + 1;
      }
   }

   // the controls the test sets, and the ones it drives as the module does
   std::vector<Param *> settable;
   Param *tPos = nullptr, *tInc = nullptr, *tRun = nullptr, *tBpm = nullptr;
   std::vector<Param *> notes, gates, trigs, vels;
   for (auto &p : ui.params)
   {
      std::string t = p.get("transport"), m = p.get("mono");
      if (t == "position") tPos = &p;
      else if (t == "increment") tInc = &p;
      else if (t == "running") tRun = &p;
      else if (t == "bpm") tBpm = &p;
      else if (!t.empty()) {}
      else if (m == "note") notes.push_back(&p);
      else if (m == "gate") gates.push_back(&p);
      else if (m == "trig") trigs.push_back(&p);
      else if (m == "velocity") vels.push_back(&p);
      else if (p.get("loop") == "restored" || p.label == "Panic" || p.get("poly") == "voice") {}
      else settable.push_back(&p);
   }
   const size_t NP = settable.size();

   std::mt19937 rng(seed * 7919u + (unsigned)strlen(name));
   std::uniform_real_distribution<float> uni(0.0f, 1.0f);
   auto snap = [](const Param &p, float v) {
      if (p.step >= 1.0f)
         v = p.min + roundf((v - p.min) / p.step) * p.step;
      return fminf(fmaxf(v, fminf(p.min, p.max)), fmaxf(p.min, p.max));
   };
   auto values = [&](int mode) {   // 0 defaults, 1 random, 2 min, 3 max
      std::vector<float> v(NP);
      for (size_t i = 0; i < NP; i++)
      {
         const Param &p = *settable[i];
         float u = uni(rng);
         float r = (p.get("scale") == "log" && p.min > 0 && p.max > p.min) ? p.min * powf(p.max / p.min, u)
                                                                            : p.min + u * (p.max - p.min);
         v[i] = mode == 0 ? p.init : mode == 1 ? snap(p, r) : mode == 2 ? p.min : p.max;
      }
      return v;
   };
   const std::vector<float> defaults = values(0);

   // inputs and outputs
   std::vector<std::vector<float>> in(nin, std::vector<float>(B)), out(nout, std::vector<float>(B));
   std::vector<float *> ip(nin), op(nout);
   for (int c = 0; c < nin; c++) ip[c] = in[c].data();
   for (int c = 0; c < nout; c++) op[c] = out[c].data();

   double pos = 0;    // transport ticks
   long t = 0;        // samples since the last reset
   float trigCount = 0;
   std::vector<int> held;

   // the program as after a start: state cleared, the memory empty
   auto reset = [&]() {
#ifdef PROG_VOICES
      poly->allNotesOff(true);
#endif
      held.clear();
      d->instanceClear();
      for (int c = 0; c < NEMESIS_MEM16_CHANNELS; c++)
         memset(nemesis::mem16::buffer[c], 0, NEMESIS_MEM16_LENGTH * sizeof(int16_t));
      pos = 0;
      t = 0;
   };

   struct Result
   {
      std::vector<Stats> out;
      long meterBad = 0;
   };
   // runs secs seconds with these settings, as the module would play them
   auto run = [&](const std::vector<float> &v, double secs, bool silent) {
      for (size_t i = 0; i < NP; i++)
         *settable[i]->zone = v[i];
      Result r;
      r.out.resize(nout);
      long n = (long)(secs * SR);
      for (long k0 = 0; k0 < n; k0 += B, t += B)
      {
         double sec = (double)t / SR;
         // transport: stopped for the first 0.5 s, then 120 BPM
         float bpm = 120.0f;
         bool running = sec >= 0.5;
         double inc = running ? 24.0 * bpm / 60.0 / SR : 0.0;
         if (tPos) *tPos->zone = (float)fmod(pos, 1536.0);
         if (tInc) *tInc->zone = (float)inc;
         if (tRun) *tRun->zone = running ? 1.0f : 0.0f;
         if (tBpm) *tBpm->zone = bpm;
         pos += inc * B;
         // notes: a new one every 0.5 s, released after 0.4 s
         long period = SR / 2, phase = t % period;
         if (silent)
         {
            for (Param *p : gates) *p->zone = 0.0f;
#ifdef PROG_VOICES
            for (int k : held) poly->keyOff(0, k, 0);
            held.clear();
#endif
         }
         else if (phase < B)
         {
            float note = 36 + (float)(rng() % 48);
            for (Param *p : notes) *p->zone = snap(*p, note);
            for (Param *p : vels) *p->zone = snap(*p, p->min + (0.3f + 0.7f * uni(rng)) * (p->max - p->min));
            for (Param *p : gates) *p->zone = 1.0f;
            trigCount += 1.0f;
            for (Param *p : trigs) *p->zone = snap(*p, trigCount);
#ifdef PROG_VOICES
            for (int i = 0; i < 3; i++)
            {
               int k = 40 + (int)(rng() % 40);
               poly->keyOn(0, k, 60 + (int)(rng() % 60));
               held.push_back(k);
            }
#endif
         }
         else if (phase >= period * 4 / 5 && phase < period * 4 / 5 + B)
         {
            for (Param *p : gates) *p->zone = 0.0f;
#ifdef PROG_VOICES
            for (int k : held) poly->keyOff(0, k, 0);
            held.clear();
#endif
         }
         // inputs: IN 1 a tone and noise bursts, IN 2 a tone, the others trigger pulses
         for (int s = 0; s < B; s++)
         {
            double ts = (double)(t + s) / SR;
            for (int c = 0; c < nin; c++)
            {
               float x = 0.0f;
               if (!silent)
               {
                  if (allTrig)
                     x = fmod(ts * (2 + c), 1.0) < 0.02 ? 0.9f : 0.0f;
                  else if (c == 0)
                  {
                     double burst = fmod(ts, 0.5);
                     x = 0.3f * sinf(2 * M_PI * 220 * ts) + (burst < 0.05 ? 0.6f * (uni(rng) * 2 - 1) * (1 - burst / 0.05) : 0.0f);
                     if (t + s == 0) x = 1.0f;
                  }
                  else if (c == 1)
                     x = 0.4f * sinf(2 * M_PI * 330 * ts);
                  else
                     x = fmod(ts * (2 + c), 1.0) < 0.02 ? 0.9f : 0.0f;
               }
               in[c][s] = x;
            }
         }
         d->compute(B, ip.data(), op.data());
         for (int c = 0; c < nout; c++)
            for (int s = 0; s < B; s++)
               r.out[c].add(out[c][s]);
         for (auto &m : ui.meters)
            if (!std::isfinite(*m.second))
               r.meterBad++;
      }
      return r;
   };
   auto failed = [&](const Result &r) {
      if (r.meterBad) return true;
      for (int c = 0; c < nout; c++)
         if (r.out[c].bad || r.out[c].peak > RUNAWAY) return true;
      return false;
   };
   auto describe = [&](const Result &r) {
      std::string s;
      char buf[96];
      for (int c = 0; c < nout; c++)
      {
         if (r.out[c].bad) { snprintf(buf, sizeof buf, " out %d NaN/inf (%ld samples)", c + 1, r.out[c].bad); s += buf; }
         else if (r.out[c].peak > RUNAWAY) { snprintf(buf, sizeof buf, " out %d peak %.3g", c + 1, r.out[c].peak); s += buf; }
      }
      if (r.meterBad) { snprintf(buf, sizeof buf, " meters NaN/inf (%ld)", r.meterBad); s += buf; }
      return s;
   };
   // which setting alone, at its value in v, makes the program fail (not
   // when the defaults fail already: then every setting would)
   bool defaultsFail = false;
   auto culprits = [&](const std::vector<float> &v) {
      if (defaultsFail) return std::string("\n      (the defaults fail already)");
      std::string s;
      char buf[160];
      int found = 0;
      for (size_t i = 0; i < NP && found < 12; i++)
      {
         if (v[i] == defaults[i]) continue;
         std::vector<float> one = defaults;
         one[i] = v[i];
         reset();
         Result r = run(one, 2.0, false);
         if (failed(r))
         {
            snprintf(buf, sizeof buf, "\n      \"%s\" = %g:%s", settable[i]->label.c_str(), v[i], describe(r).c_str());
            s += buf;
            found++;
         }
      }
      if (!s.empty()) return s;
      // a combination: which settings, put back to their defaults one at a
      // time, make it pass (each of them is needed for the failure)
      s = "\n      no single setting; needed for the failure (back at its default it passes):";
      found = 0;
      for (size_t i = 0; i < NP && found < 12; i++)
      {
         if (v[i] == defaults[i]) continue;
         std::vector<float> most = v;
         most[i] = defaults[i];
         reset();
         if (!failed(run(most, 2.0, false)))
         {
            snprintf(buf, sizeof buf, "\n      \"%s\" = %g (default %g)", settable[i]->label.c_str(), v[i], defaults[i]);
            s += buf;
            found++;
         }
      }
      if (!found) s += "\n      none alone (several ways to fail)";
      return s;
   };

   int failures = 0, warnings = 0;
   printf("== %s: %d in, %d out, %d Hz, block %d, %zu settings\n", name, nin, nout, SR, B, NP);
   auto show = [&](const char *ph, const Result &r) {
      printf("  %-9s", ph);
      for (int c = 0; c < nout; c++)
         printf(" %s%d %.2f/%.3f", audio[c] ? "out" : "cv", c + 1, r.out[c].peak, r.out[c].rms());
      printf("   (peak/rms)\n");
   };

   auto loud = [&](const char *ph, const Result &r) {
      std::string s;
      char buf[48];
      for (int c = 0; c < nout; c++)
         if (r.out[c].peak > LOUD && r.out[c].peak <= RUNAWAY && r.out[c].bad == 0)
         {
            snprintf(buf, sizeof buf, " out %d %.1f", c + 1, r.out[c].peak);
            s += buf;
         }
      if (!s.empty())
      {
         printf("  warn %s: louder than %.0f x full scale:%s\n", ph, LOUD, s.c_str());
         warnings++;
      }
   };

   // defaults: the program as it starts
   reset();
   Result def = run(defaults, 4.0, false);
   show("defaults", def);
   if (failed(def))
   {
      printf("  FAIL defaults:%s\n", describe(def).c_str());
      failures++;
      defaultsFail = true;
   }
   for (int c = 0; c < nout; c++)
      if (audio[c] && def.out[c].peak > LOUD && def.out[c].peak <= RUNAWAY)
      {
         printf("  FAIL defaults: output %d peaks at %.1f (more than %.0f x full scale)\n", c + 1, def.out[c].peak, LOUD);
         failures++;
      }
   for (int c = 0; c < nout; c++)
   {
      if (!audio[c]) continue;
      const Stats &s = def.out[c];
      if (fabs(s.dc()) > 0.05)
      {
         printf("  warn defaults: output %d DC offset %.3f\n", c + 1, s.dc());
         warnings++;
      }
      if (s.n && (double)s.clipped / s.n > 0.01)
      {
         printf("  warn defaults: output %d beyond 1.0 in %.1f %% of the samples\n", c + 1, 100.0 * s.clipped / s.n);
         warnings++;
      }
   }
   {
      bool sounds = false;
      for (int c = 0; c < nout; c++)
         if (audio[c] && def.out[c].rms() > 1e-4) sounds = true;
      if (!sounds)
      {
         printf("  warn defaults: every audio output is silent\n");
         warnings++;
      }
   }

   // random settings, one after the other without a reset (as turning knobs)
   Result rnd;
   rnd.out.resize(nout);
   int randomFailures = 0;
   std::vector<std::vector<float>> sets;
   for (int k = 0; k < 24; k++)
      sets.push_back(values(1));
   for (int k = 0; k < 24; k++)
   {
      Result r = run(sets[k], 1.0, false);
      if (getenv("TRACE"))   // TRACE=1: every random set's peaks and settings
      {
         printf("  set %2d:", k + 1);
         for (int c = 0; c < nout; c++) printf(" %.2f", r.out[c].peak);
         printf("\n        ");
         for (size_t i = 0; i < NP; i++)
            if (sets[k][i] != defaults[i]) printf(" %s=%g", settable[i]->label.c_str(), sets[k][i]);
         printf("\n");
      }
      for (int c = 0; c < nout; c++)
      {
         rnd.out[c].peak = fmax(rnd.out[c].peak, r.out[c].peak);
         rnd.out[c].sum2 += r.out[c].sum2;
         rnd.out[c].n += r.out[c].n;
      }
      if (failed(r))
      {
         randomFailures++;
         printf("  FAIL random set %d:%s%s\n", k + 1, describe(r).c_str(), culprits(sets[k]).c_str());
         failures++;
         reset();   // the next set starts clean (the module would reset a NaN program)
      }
   }
   show("random", rnd);
   loud("random", rnd);

   // extremes
   for (int mode : {2, 3})
   {
      const char *ph = mode == 2 ? "min" : "max";
      std::vector<float> v = values(mode);
      reset();
      Result r = run(v, 2.0, false);
      show(ph, r);
      loud(ph, r);
      if (failed(r))
      {
         printf("  FAIL %s:%s%s\n", ph, describe(r).c_str(), culprits(v).c_str());
         failures++;
      }
   }

   // recovery: the max settings, then defaults with silent inputs; a NaN
   // that stays means the program can't recover without a reset
   {
      reset();
      run(values(3), 2.0, false);
      Result r = run(defaults, 4.0, true);
      show("recovery", r);
      bool stuck = false;
      for (int c = 0; c < nout; c++)
         if (r.out[c].bad) stuck = true;
      if (stuck)
      {
         printf("  FAIL recovery: still NaN/inf at the defaults after the max settings (stays until a reset)\n");
         failures++;
      }
      double tail = 0;
      for (int c = 0; c < nout; c++)
         if (audio[c]) tail = fmax(tail, r.out[c].rms());
      if (tail > 0.3 && nin > 0)   // a synth may sound on its own (a drone)
      {
         printf("  warn recovery: audio still at rms %.2f with silent inputs and defaults\n", tail);
         warnings++;
      }
   }

   printf("  %s: %d failures, %d warnings\n", failures ? "FAILED" : "ok", failures, warnings);
   delete d;
   return failures ? 1 : 0;
}
