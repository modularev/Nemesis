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
#include "nemesis_transport_core.h"

#ifndef NEMESIS_HW_REV
#error "Set the hardware revision: build_flags = -D NEMESIS_HW_REV=1 or 2 (see software/README.md)"
#elif NEMESIS_HW_REV != 1 && NEMESIS_HW_REV != 2
#error "NEMESIS_HW_REV must be 1 or 2"
#endif

// audio blocks for Audio library objects; the programs read and write the
// TDM DMA buffers directly (nemesis_tdm.h) and need none
#ifndef NEMESIS_AUDIO_MEMORY
#define NEMESIS_AUDIO_MEMORY 4
#endif

#if NEMESIS_HW_REV == 2
#include <Adafruit_SSD1306.h>
#ifndef NEMESIS_ENCODER_COUNTS_PER_STEP
#define NEMESIS_ENCODER_COUNTS_PER_STEP 4
#endif
#endif

namespace nemesis
{
   constexpr int NUM_OUTPUTS = 8;

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

   // start the hardware and run the program on the codec's TDM in- and outputs,
   // audio memory is set with -D NEMESIS_AUDIO_MEMORY=<blocks> (default 4)
   void begin(Program &program);
   // audio block size, latency and late output writes (nemesis_tdm.h)
   void printAudioStatus(Print &out);
   // the block size in use, and the one the setting and program ask for
   int blockSize();
   int wantedBlockSize();
   // the sample rate in use (48000 or 96000), and the one the setting and a
   // program ('declare sample_rate "96000";', null: the running one) ask for
   int sampleRate();
   int wantedSampleRate(const Program *program);
   // applySampleRate(): see nemesis_program.h
   // CS42448 register 03h for the sample rate: MFREQ of MCLK = 512 fs
   uint8_t codecFunctionalMode();
   // CPU load of the audio update in % of the block's time (the Audio
   // library's figures assume AUDIO_BLOCK_SAMPLES)
   float cpuLoad();
   float cpuLoadMax();

   // several programs in one firmware: the last selected one is restored at
   // power up, on rev2 the encoder selects (turn) and loads (push) a program
   void begin(Program *const *programs, int count);
   template <size_t N>
   void begin(Program *(&programs)[N]) { begin(programs, N); }

   bool selectProgram(int index);
   int currentProgram();
   int programCount();
   Program *program(int index);

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

   // system settings, kept on the SD card (see nemesis_settings.cpp)
   struct SystemSettings
   {
      int midiChannel = 0;   // 0 omni, 1..16
      bool popup = true;     // rev2: show the parameter of a turned knob
      int brightness = 10;   // rev2: display, 1..10
      int cvRangeVolts = 8;  // rev2: CV A-D read 0..1 = 0..5 V or 0..8 V
      // transport
      int clockSource = 0;   // TransportCore::AUTO (MIDI clock when there is one), INTERNAL, MIDI, CV (audio IN 8)
      float tempo = 120;     // internal clock, BPM
      bool run = true;       // also switched by MIDI Start/Stop
      int clockOffsetMs = 0; // shifts the transport position, -50..+50 ms
      int clockPpqn = 4;     // clock on IN 8: pulses per quarter, 1, 2, 4 (sixteenths) or 24
      int blockSize = 0;     // audio block: 0 auto (the program's, else 64), 32, 64, 128;
                             // not stored: every power up starts with auto
      int sampleRate = 0;    // 0 auto (the program's, else 48000), 48000, 96000; not stored
   };

   SystemSettings &systemSettings();
   // schedule saving to the SD card
   void systemSettingsChanged();
   void programSettingsChanged(Program &program);
   bool sdCardPresent();
   // why the SD card is not in use: "no SD" (missing), "SD off" (left alone
   // after a hang at start), "SD bad" (a damaged file); nullptr when in use
   const char *sdProblem();

   // a hardware watchdog resets the module when loop() stops for 4 s
   // (feed it in long blocking work); starts that end before 10 s are
   // counted, after SAFE_AFTER_FAILURES of them the next one starts
   // program 1 and leaves the SD card alone
   constexpr int SAFE_AFTER_FAILURES = 2;
   void watchdogBegin();
   void watchdogFeed();
   int bootFailures();

   // CV inputs whose routing to a parameter can be changed (rev2: CV A-D)
#if NEMESIS_HW_REV == 2
   constexpr int NUM_ROUTABLE_CV = 4;
#else
   constexpr int NUM_ROUTABLE_CV = 0;
#endif
   int routableCV(int i); // input index

   // transport: position in MIDI clock ticks (24 per quarter) on the internal
   // tempo or the MIDI clock, see nemesis_transport_core.h; FAUST programs
   // get it with nemesis.lib
   const TransportState &transportState();
   namespace transport
   {
      // advance by one audio block, called by the program before computing it
      const TransportState &block();
   }

#if NEMESIS_HW_REV == 2
   extern Adafruit_SSD1306 display;
   // the library shows the program on the display and runs the program list
   // on the encoder; disable it to draw your own screen
   void showMonitor(bool enable);

   // encoder position in detents (counted in interrupts) and its push switch
   int32_t encoderPosition();
   bool encoderPressed();

   // CV input (0..6 = X Y Z A B C D) in volts; calibrated, input() reads
   // 0..1 = 0..8 V for A-D and -4..+4 V for X/Y/Z
   float cvVolts(int index);
   // volt calibration, measured with tests/cvtest: raw reading at 0 V and per
   // volt of a CV input, volts per unit and at 0 of an output
   void setCvCalibration(int index, float zeroReading, float readingPerVolt);
   void setOutputCalibration(int output, float voltsPerUnit, float offsetVolts);
   void storeVoltCalibration(); // to EEPROM
   void resetVoltCalibration(); // design values, not stored
   void printVoltCalibration(Stream &out = Serial);
#endif
}

#endif
