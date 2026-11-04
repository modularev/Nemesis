//
// Library for the programmable Eurorack DSP Module NEMESIS
//
// nemesis_transport_core.h
//
// The module's transport: a position in MIDI clock ticks (24 per quarter)
// that runs on the internal tempo or follows the MIDI clock. Plain C++
// without hardware access, so it can be tested on a computer; the hardware
// side is nemesis_transport.cpp.
//
// MIDI clock: every tick is timestamped (in samples) when it arrives. A
// phase locked loop runs the position at the measured tempo and pulls it
// gently towards the ticks, so jitter in the arrival times doesn't reach
// the position. The position runs at most one tick past the next expected
// tick: when the clock stops without a MIDI Stop, the position holds.
// MIDI Start: the first clock after it is position 0. Continue resumes,
// Song Position sets the position while stopped.
//
// Copyright © 2021 Arev Imer if not mentioned elsewise
// released under GNU GPL v3
//

#ifndef nemesis_transport_core_h
#define nemesis_transport_core_h

#include <cmath>

namespace nemesis
{
   struct TransportState
   {
      double position = 0;   // ticks at the start of the block, wrapped to WRAP
      double increment = 0;  // ticks per sample during the block, 0 while stopped
      bool running = false;
      bool external = false; // following the MIDI clock
      float bpm = 120;
   };

   class TransportCore
   {
   public:
      enum Source { AUTO = 0, INTERNAL = 1, MIDI = 2 };
      static constexpr double TICKS_PER_QUARTER = 24;
      static constexpr double WRAP = 1536; // 16 bars, a multiple of every note value

      // settings, read on every block
      double sampleRate = 48000;
      int source = AUTO;
      double tempo = 120;         // internal clock, BPM
      double offsetSamples = 0;   // position shifted by this (latency compensation)

      // loop gains per tick: phase error is reduced by KP per tick, KI follows tempo changes
      double KP = 0.5;
      double KI = 0.1;
      // how far the position may run past the next expected tick: ticks are
      // only seen at the next block, and a stopped clock holds after this
      double LEAD = 1;

      // MIDI events, t in samples on the same time base as block()
      void clock(double t)
      {
         bool wasActive = lastTick >= 0 && t - lastTick < clockTimeout();
         if (!wasActive && !pendingStart)
            resync = true; // a clock (re)appears: join it from the current position
         if (lastTick >= 0)
         {
            double d = t - lastTick;
            if (d > 0 && d < clockTimeout())
               interval = (interval > 0) ? interval + 0.25 * (d - interval) : d;
         }
         lastTick = t;
         if (!usesMidi() || !running)
            return; // the tempo is still measured
         if (pendingStart)
         {
            pendingStart = false;
            expected = 0;
            resync = true;
         }
         else if (resync)
         {
            expected = std::ceil(outPos - 1e-6); // join a running clock without a jump back
         }
         if (interval <= 0)
         {
            pos = outPos = expected; // first tick: wait for a tempo
            anchor = t;
            inc = 0;
            locked = false;
            resync = false;
            expected += 1;
            return;
         }
         double raw = pos + (t - anchor) * inc;
         double err = expected - raw;
         if (resync || !locked || std::fabs(err) > 2)
         {
            pos = expected;
            freq = 1 / interval;
            inc = freq;
            locked = true;
            resync = false;
         }
         else
         {
            pos = std::fmin(raw, expected + LEAD);
            freq *= 1 + KI * err;
            inc = freq * (1 + KP * err);
         }
         anchor = t;
         expected += 1;
      }

      void start()
      {
         if (source == INTERNAL)
            return;
         running = true;
         pendingStart = true;
         outPos = 0;
         pos = 0;
         inc = 0;
      }

      void cont()
      {
         if (source == INTERNAL)
            return;
         running = true;
         resync = true;
      }

      void stop()
      {
         if (source == INTERNAL)
            return;
         running = false;
      }

      // Song Position Pointer: sixteenth notes, while stopped
      void songPosition(int sixteenths)
      {
         if (source == INTERNAL || running)
            return;
         outPos = sixteenths * 6.0;
         pos = outPos;
         expected = outPos;
         resync = true;
      }

      // run on/off from the menu
      void setRunning(bool run)
      {
         if (run == running)
            return;
         running = run;
         pendingStart = false;
         resync = true;
      }
      bool isRunning() const { return running; }

      // position at the start of a block of n samples at time t
      const TransportState &block(double t, int n)
      {
         bool midiActive = lastTick >= 0 && t - lastTick < clockTimeout();
         if (!midiActive && locked)
         {
            locked = false;
            resync = true; // rejoin when the clock comes back
         }
         bool external = (source == MIDI) || (source == AUTO && midiActive);
         if (source != lastSource)
            resync = true; // join the other clock from the current position
         lastSource = source;
         state.external = external;

         double increment = 0;
         if (external)
         {
            if (running && locked && !pendingStart)
            {
               double raw = pos + (t - anchor) * inc;
               outPos = std::fmin(raw, expected + LEAD); // hold when the clock stops
               increment = std::fmax(0.0, std::fmin(inc, (expected + LEAD - outPos) / n));
            }
            state.bpm = interval > 0 ? (float)(sampleRate * 60 / (TICKS_PER_QUARTER * interval)) : 0;
         }
         else
         {
            if (running)
               increment = tempo * TICKS_PER_QUARTER / 60 / sampleRate;
            state.bpm = (float)tempo;
         }

         state.running = running && !(external && (pendingStart || !locked));
         state.increment = increment;
         double p = std::fmod(outPos + offsetSamples * increment, WRAP);
         state.position = p < 0 ? p + WRAP : p;
         outPos += increment * n;
         if (outPos >= 2 * WRAP)
         {
            double shift = std::floor(outPos / WRAP) * WRAP - WRAP;
            outPos -= shift; // keep the numbers small, the shift is a multiple of WRAP
            pos -= shift;
            expected -= shift;
         }
         return state;
      }

      const TransportState &current() const { return state; }

   private:
      bool usesMidi() const { return source != INTERNAL; }
      double clockTimeout() const { return sampleRate * 0.3; } // 30 BPM has 83 ms per tick

      TransportState state;
      int lastSource = AUTO;
      bool running = true;
      bool pendingStart = false;
      bool locked = false;
      bool resync = true;
      double lastTick = -1;  // time of the last MIDI clock
      double interval = 0;   // averaged samples per tick
      double pos = 0;        // MIDI position at anchor
      double anchor = 0;
      double inc = 0;        // ticks per sample until the next tick
      double freq = 0;       // loop tempo, ticks per sample
      double expected = 0;   // index of the next MIDI tick
      double outPos = 0;     // position at the start of the next block (unwrapped)
   };
}

#endif
