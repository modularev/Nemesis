// juno_levels.cpp - evens out the loudness of juno presets
//
// Plays every line of a juno.PRE (software/tools/juno106_presets.py) with
// juno's own code on the computer: one note (C4, velocity 100) held 1.5 s
// through a voice and the effect (HPF, chorus, limiter), measures its
// loudest 50 ms (RMS: fair to short hits and slow swells alike) and sets
// the preset's Level so that all reach the same loudness (Level stays
// within 0.05 .. 2; 0.5 is unity). Writes the file back.
//
// build (from software/tools):
//   faust -cn junovoice -i ../programs/juno.dsp -o /tmp/junovoice.h
//   faust -cn junoeffect -pn effect -i ../programs/juno.dsp -o /tmp/junoeffect.h
//   c++ -O2 -std=c++17 -I/tmp -I/opt/homebrew/include juno_levels.cpp -o /tmp/juno_levels
//   /tmp/juno_levels juno.PRE [target_rms]
//
// Copyright © 2021 Arev Imer if not mentioned elsewise
// released under GNU GPL v3

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#define FAUSTFLOAT float
#include "faust/dsp/dsp.h"
#include "faust/gui/MapUI.h"
#include "faust/gui/meta.h"
#include "junovoice.h"
#include "junoeffect.h"

static const int SR = 48000;

// "name|label=value|..." -> name and pairs
static void parse(const std::string &line, std::string &name, std::vector<std::pair<std::string, std::string>> &values)
{
   std::stringstream ss(line);
   std::string field;
   std::getline(ss, name, '|');
   while (std::getline(ss, field, '|'))
   {
      size_t eq = field.find('=');
      if (eq != std::string::npos)
         values.push_back({field.substr(0, eq), field.substr(eq + 1)});
   }
}

static double render(const std::vector<std::pair<std::string, std::string>> &values, double level)
{
   junovoice voice;
   junoeffect effect;
   voice.init(SR);
   effect.init(SR);
   MapUI vu, eu;
   voice.buildUserInterface(&vu);
   effect.buildUserInterface(&eu);
   for (auto &v : values)
   {
      float x = atof(v.second.c_str());
      if (v.first == "Level")
         x = level;
      vu.setParamValue(v.first, x); // a label the voice doesn't have is ignored
      eu.setParamValue(v.first, x);
   }
   vu.setParamValue("drift", 0);
   vu.setParamValue("freq", 261.63f);
   vu.setParamValue("gain", 100 / 127.0f);
   vu.setParamValue("gate", 1);
   const int n = 64, total = SR * 3 / 2, window = SR / 20;
   std::vector<float> vl(n), vr(n), el(n), er(n);
   float *vo[2] = {vl.data(), vr.data()}, *eo[2] = {el.data(), er.data()};
   float *in[2] = {nullptr, nullptr};
   double sum = 0, loudest = 0;
   int counted = 0;
   for (int k = 0; k < total; k += n)
   {
      voice.compute(n, in, vo);
      effect.compute(n, vo, eo);
      for (int i = 0; i < n; i++)
      {
         sum += 0.5 * (el[i] * el[i] + er[i] * er[i]);
         if (++counted == window)
         {
            loudest = std::fmax(loudest, sum / window);
            sum = 0;
            counted = 0;
         }
      }
   }
   return sqrt(loudest);
}

int main(int argc, char **argv)
{
   if (argc < 2)
   {
      fprintf(stderr, "usage: juno_levels juno.PRE [target_rms]\n");
      return 1;
   }
   double target = argc > 2 ? atof(argv[2]) : 0.12;
   std::ifstream in(argv[1]);
   std::vector<std::string> lines;
   std::string line;
   while (std::getline(in, line))
      lines.push_back(line);
   int changed = 0;
   for (auto &l : lines)
   {
      if (l.empty() || l[0] == '#')
         continue;
      std::string name;
      std::vector<std::pair<std::string, std::string>> values;
      parse(l, name, values);
      double rms = render(values, 0.5); // at unity
      double level = rms > 1e-6 ? 0.5 * target / rms : 1.0;
      level = std::fmax(0.05, std::fmin(2.0, level));
      double after = render(values, level);
      printf("%-26s rms %.3f -> Level %.3f (%.3f)\n", name.c_str(), rms, level, after);
      char text[16];
      snprintf(text, sizeof(text), "%.3f", level);
      bool found = false;
      for (auto &v : values)
         if (v.first == "Level")
         {
            v.second = text;
            found = true;
         }
      if (!found)
         values.push_back({"Level", text});
      std::string out = name;
      for (auto &v : values)
         out += "|" + v.first + "=" + v.second;
      l = out;
      changed++;
   }
   std::ofstream o(argv[1]);
   for (auto &l : lines)
      o << l << "\n";
   printf("%d presets levelled to rms %.3f\n", changed, target);
   return 0;
}
