//
// Library for the programmable Eurorack DSP Module NEMESIS
//
// nemesis.h
//
// One API for both hardware revisions. Select the revision at build time
// with -D NEMESIS_HW_REV=1 or -D NEMESIS_HW_REV=2 (see the [env:rev1] and
// [env:rev2] sections of the example platformio.ini files).
//
//   rev1: 9 knobs, each summed with its CV jack, read on Teensy analog pins
//         inputs CV_X CV_Y CV_Z CV_A .. CV_F (POT_X .. POT_F are aliases)
//   rev2: 7 CV jacks on an LTC1867 SPI ADC, 4 knobs, 128x32 OLED
//         inputs CV_X CV_Y CV_Z CV_A CV_B CV_C CV_D POT_A POT_B POT_C POT_D
//
// Copyright © 2021 Arev Imer if not mentioned elsewise
// released under GNU GPL v3
//

#ifndef nemesis_h
#define nemesis_h

#include <Arduino.h>
#include <Audio.h>
#include "nemesis_program.h"

#ifndef NEMESIS_HW_REV
#error "Set the hardware revision: build_flags = -D NEMESIS_HW_REV=1 or 2 (see software/README.md)"
#elif NEMESIS_HW_REV != 1 && NEMESIS_HW_REV != 2
#error "NEMESIS_HW_REV must be 1 or 2"
#endif

#ifndef NEMESIS_AUDIO_MEMORY
#define NEMESIS_AUDIO_MEMORY 100
#endif

#if NEMESIS_HW_REV == 2
#include <Adafruit_SSD1306.h>
#ifndef NEMESIS_ENCODER_COUNTS_PER_STEP
#define NEMESIS_ENCODER_COUNTS_PER_STEP 4
#endif
#endif

namespace nemesis
{
#if NEMESIS_HW_REV == 1
   constexpr int NUM_INPUTS = 9;
   constexpr int NUM_LEDS = 3;
   constexpr int NUM_BUTTONS = 3;
#else
   constexpr int NUM_INPUTS = 11;
   constexpr int NUM_LEDS = 2;
   constexpr int NUM_BUTTONS = 2;
#endif

   extern AudioControlCS42448 codec;

   // start the hardware and patch the program between the codec TDM in- and outputs,
   // audio memory is set with -D NEMESIS_AUDIO_MEMORY=<blocks> (default 100)
   void begin(Program &program);

   // scan the inputs and write the bound program parameters, call it from loop()
   void update();

   // current value of a hardware input, 0..1
   float input(int index);
   // the same without calibration, e.g. to measure CV input voltages
   float rawInput(int index);
   const char *inputName(int index);
   // index of an input name like "CV_X" on this revision, -1 if it doesn't exist
   int inputIndex(const char *name);

   void setLed(int index, bool on);
   bool buttonPressed(int index);

   // print the program name and its bound parameters
   void printControls(Stream &out = Serial);

   // write a CS42448 register (AudioControlCS42448::write() is private)
   bool codecWrite(uint8_t reg, uint8_t val);

#if NEMESIS_HW_REV == 2
   extern Adafruit_SSD1306 display;
   // the library redraws an input monitor on the display, disable it to draw your own
   void showMonitor(bool enable);

   // encoder position in detents (counted in interrupts) and its push switch
   int32_t encoderPosition();
   bool encoderPressed();
#endif
}

#endif
