//
// Library for the programmable Eurorack DSP Module NEMESIS
//
// nemesis_hw.h
//
// Internal interface implemented once per hardware revision
// (nemesis_rev1.cpp, nemesis_rev2.cpp). Not meant to be used by sketches.
//
// Copyright © 2021 Arev Imer if not mentioned elsewise
// released under GNU GPL v3
//

#ifndef nemesis_hw_h
#define nemesis_hw_h

#include "nemesis.h"

namespace nemesis
{
   namespace hw
   {
      extern const char *const inputNames[NUM_INPUTS];

      // pins, control ADCs, display, audio memory and codec
      void begin(const Program &program);
      // read the next control input(s), called on every nemesis::update()
      void scan();
      // slow periodic work like display refreshes
      void service(const Program &program);

      float input(int index);
      float rawInput(int index);
      void resetVoltCalibration();
      // revision specific alternative input names, -1 if none matches
      int aliasIndex(const char *name);

      void setLed(int index, bool on);
      bool buttonPressed(int index);

      // FlexSPI2 clock for the optional PSRAM
      void setPSRAMspeed();
   }
}

#endif
