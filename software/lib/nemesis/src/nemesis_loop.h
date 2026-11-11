//
// Library for the programmable Eurorack DSP Module NEMESIS
//
// nemesis_loop.h - a program's frozen loop memory on the SD card
//
// A program that keeps a loop in delay lines (pollen: its live memory)
// marks it with metadata:
//
//   hbargraph("loop [loop:length]", 0, N)     the frozen loop's length in
//                                            samples, 0 while recording
//   hslider("freeze [loop:freeze] ...")       its freeze setting
//   hslider("loop length [loop:restored]")    hidden: the library writes the
//                                            length of a loop it loaded
//
// When the length stays above 0 for half a second, the library writes the
// loop to /NEMESIS/<program>.WAV (32 bit float, one channel per large delay
// line, oldest sample first). When the program starts again with freeze on,
// the file goes back into the delay lines before the program's first block,
// where the program continues the loop.
//
// The delay lines are FAUST's (>= 256 kB, found by the architecture's memory
// manager); each is written once per sample at (samples computed) mod its
// length, so the newest sample's place is known without FAUST's internals.
// Saving runs in loop() (never in the audio interrupt) and reads the oldest
// samples first: the program overwrites them only after (length - loop)
// samples, about a second for the longest loop.
//
// Copyright © 2021 Arev Imer if not mentioned elsewise
// released under GNU GPL v3
//

#ifndef nemesis_loop_h
#define nemesis_loop_h

#include <Arduino.h>
#include "nemesis_program.h"

namespace nemesis
{
   namespace loop
   {
      // from the library's update(): saves a loop that was just frozen
      void service(Program &program);
      // after start() and the settings, before the program runs: loads the
      // saved loop if the program is frozen
      void restore(Program &program);
   }
}

#endif
