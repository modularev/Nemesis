//
// main file for the programmable Eurorack DSP Module NEMESIS by Arev Imer
//
// src/main.cpp
//
// Copyright © 2021 Arev Imer if not mentioned elsewise
// released under GNU GPL v3
//

// include standard library header files
#include <Audio.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <SerialFlash.h>
#include <elapsedMillis.h>

// include program-defined header files
#include "nemesis.h"
#include "FAUST.h"

//
// Audio Connections
// sends physical audio inputs 1-8 to faust and digital outputs to
//
AudioInputTDM audio_in;
AudioOutputTDM audio_out;
FAUST faustObj; // replace with library name of your faust export
AudioConnection IN_1(audio_in, 0, faustObj, 0);
AudioConnection IN_2(audio_in, 2, faustObj, 1);
AudioConnection IN_3(audio_in, 4, faustObj, 2);
AudioConnection IN_4(audio_in, 6, faustObj, 3);
AudioConnection IN_5(audio_in, 8, faustObj, 4);
AudioConnection IN_6(audio_in, 10, faustObj, 5);
AudioConnection IN_7(audio_in, 12, faustObj, 6);
AudioConnection IN_8(audio_in, 14, faustObj, 7);
AudioConnection OUT_1(faustObj, 0, audio_out, 0);
AudioConnection OUT_2(faustObj, 1, audio_out, 2);
AudioConnection OUT_3(faustObj, 2, audio_out, 4);
AudioConnection OUT_4(faustObj, 3, audio_out, 6);
AudioConnection OUT_5(faustObj, 4, audio_out, 8);
AudioConnection OUT_6(faustObj, 5, audio_out, 10);
AudioConnection OUT_7(faustObj, 6, audio_out, 12);
AudioConnection OUT_8(faustObj, 7, audio_out, 14);
AudioControlCS42448 codec;

// IntervalTimer myTimer; // Timer for audio usage

//
// Parameters from Faust
//
const char *param[] = {
    "POT X",
    "POT Y",
    "POT Z",
    "POT A",
    "POT B",
    "POT C",
    "POT D",
    "POT E",
    "POT F"};

//
// Variables
//
uint16_t adc_new[9];
uint16_t adc_old[9];
float fValue[9];

constexpr float smoothing = 0.2f;

elapsedMicros hysteresis[9];

//
// function called by IntervalTimer to observe processor and memory usage
//
// void print_audio_usage()
// {
//    Serial.print(AudioProcessorUsage());
//    Serial.print("\t");
//    Serial.print(faustObj.processorUsage());
//    Serial.print("\t");

//    for (int i = 0; i < 9; i++)
//    {
//       Serial.print(adc_new[i]);
//       Serial.print("\t");
//    }
//    Serial.println(AudioMemoryUsageMax());
// }

void update()
{
   for (int i = 0; i < 9; i++)
   {
      adc_new[i] = adc_new[i] + smoothing * (analogRead(analog_pin[i]) - adc_new[i]);
      if (abs(adc_new[i] - adc_old[i]) > 5 || hysteresis[i] < 1000)
      {
         send_params(i);
         adc_old[i] = adc_new[i];
         hysteresis[i] = 0;
         Serial.print(i);
         Serial.print("\t");
         Serial.println(fValue[i], 3);
      }
   }
}

void send_params(int i)
{ // !!!RANGE IS INVERTED!!!
   fValue[i] = -(float)(adc_new[i] - nemesis::getADC_min(i)) / (float)(nemesis::getADC_min(i) - nemesis::getADC_max(i));
   faustObj.setParamValue(param[i], clamp(fValue[i]));
}

void setup()
{
   Serial.begin(9600);

   nemesis::init();
   // nemesis::setSampleRate(96000000);
   nemesis::calibration(); // calibration routine

   if (EEPROM.read(0) == 0xFF) // check for calbration flag
   {
      nemesis::loadCalibration();
      Serial.println("Calibration loaded!");
   }
   else
      Serial.println("No calibration found!");

   AudioMemory(40);
   delay(10);
   codec.enable();
   delay(10);
   codec.volume(0.01);
   codec.invertDAC(0x3F);
   codec.invertADC(0x3F);
   delay(100); // wait for dc offset to stabilize
   codec.filterFreeze();

   //myTimer.begin(print_audio_usage, 500000);
}

void loop()
{
   update();
}