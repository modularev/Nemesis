// SPDX-License-Identifier: GPL-3.0-or-later
// Host test of lib/nemesis/src/nemesis_transport_core.h: tempo of the CV
// clock and the MIDI clock, transients, a clock that stops.
//   tests/transport/run.sh
#include "../../lib/nemesis/src/nemesis_transport_core.h"
#include <cstdio>
#include <cmath>

using namespace nemesis;
static int failures = 0;
static void check(bool ok, const char *what, double got)
{
   printf("%s %-58s %.2f\n", ok ? "ok  " : "FAIL", what, got);
   failures += !ok;
}

// runs blocks of 64 samples up to time `until`, firing clock() at the times given
static void run(TransportCore &c, double &t, double until, double period, double first = -1)
{
   double next = first >= 0 ? first : t;
   while (t < until)
   {
      while (period > 0 && next < t + 64)
      {
         c.clock(next);
         next += period;
      }
      c.block(t, 64);
      t += 64;
   }
}

int main()
{
   const double SR = 48000;
   {
      // CV clock at 24 ppqn, 120 BPM: a pulse every 1000 samples
      TransportCore c;
      c.source = TransportCore::CV;
      c.tickSize = 1;
      c.setRunning(true);
      double t = 0;
      run(c, t, SR * 4, SR * 60 / (120 * 24));
      check(std::fabs(c.current().bpm - 120) < 0.5, "CV clock 24 ppqn at 120 BPM", c.current().bpm);
      check(c.current().running, "CV clock runs", c.current().running);
      // a transient: two extra edges 2 samples apart in between
      c.clock(t + 10);
      c.clock(t + 12);
      run(c, t, t + SR, SR * 60 / (120 * 24), t + 500);
      check(std::fabs(c.current().bpm - 120) < 2, "tempo after a 2-sample transient", c.current().bpm);
      // the clock stops: the internal tempo
      c.tempo = 97;
      run(c, t, t + SR * 3, 0);
      check(std::fabs(c.current().bpm - 97) < 0.01, "no clock any more: internal tempo", c.current().bpm);
   }
   {
      // only noise edges, no clock: never a fast tempo
      TransportCore c;
      c.source = TransportCore::CV;
      c.setRunning(true);
      double t = 0;
      for (int i = 0; i < 20; i++)
         c.clock(1000 + i * 2.25);
      run(c, t, SR * 3, 0);
      check(c.current().bpm < 1000, "edges 2 samples apart don't make a tempo", c.current().bpm);
   }
   {
      // MIDI clock bunched: 3 ticks at almost the same time, every 3000 samples (= 120 BPM)
      TransportCore c;
      c.source = TransportCore::MIDI;
      double t = 0;
      c.start();
      double next = 0;
      while (t < SR * 4)
      {
         while (next < t + 64)
         {
            c.clock(next);
            c.clock(next + 1);
            c.clock(next + 2);
            next += 3000;
         }
         c.block(t, 64);
         t += 64;
      }
      double pos = c.current().position;
      check(std::fabs(pos - 4 * 2 * 24) < 6, "bunched MIDI clock: all ticks count (position after 4 s)", pos);
   }
   printf(failures ? "%d FAILED\n" : "all passed\n", failures);
   return failures != 0;
}
