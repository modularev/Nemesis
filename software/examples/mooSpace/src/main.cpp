#include <Audio.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <SerialFlash.h>
#include <elapsedMillis.h>
#include "nemesis.h"
#include "mooSpace.h"

//
// Audio Connections
//
AudioInputTDM audio_in;
AudioOutputTDM audio_out;
mooSpace faustObj;
AudioConnection IN_L(audio_in, 0, faustObj, 0);
AudioConnection IN_R(audio_in, 2, faustObj, 1);
AudioConnection OUT_L(faustObj, 0, audio_out, 0);
AudioConnection OUT_R(faustObj, 1, audio_out, 2);
AudioControlCS42448 codec;

IntervalTimer myTimer;

//
// Parameters from Faust
//
const char *param[] = {
    "smear",   // POT X
    "decay",   // POT Y
    "drag",    // POT Z
    "Filter",// POT A
    "Filter", // POT B
    "colour",  // POT C
    "lag",     // POT D
    "push",    // POT E
    "mix"};    // POT F

//
// Variables
//
uint16_t adc_new[9];
uint16_t adc_old[9];
float fValue[9];

constexpr float smoothing = 0.5f;

elapsedMicros hysteresis[9];

// 
// 
// 
void print_audio_usage()
{
   Serial.print(AudioProcessorUsage());
   Serial.print("\t");
   Serial.print(faustObj.processorUsage());
   Serial.print("\t");

   for (int i = 0; i < 9; i++)
   {
      Serial.print(adc_new[i]);
      Serial.print("\t");
   }
   Serial.println(AudioMemoryUsageMax());
}

void send_params(int i)
{ // !!!RANGE IS INVERTED!!!
   fValue[i] = -(float)(adc_new[i] - nemesis::getADC_min(i)) / (float)(nemesis::getADC_min(i) - nemesis::getADC_max(i));
   faustObj.setParamValue(param[i], clamp(fValue[i]));
}


void setup()
{
   Serial.begin(9600);
//    while (!Serial) {
//     ; // wait for serial port to connect. Needed for native USB
//   }

   nemesis::init();
   // nemesis::setSampleRate(96000000);
   nemesis::calibration(); // calibration routine

   if (EEPROM.read(0) == 0xFF) // check for calbration flag
   {
      nemesis::loadCalibration();
      Serial.println("Calibration loaded!");
   }
   else Serial.println("No calibration found!");

   AudioMemory(40); //832 max
   delay(10);
   codec.enable();
   delay(10);
   codec.volume(0.8);
   //codec.invertDAC(0x3F);
   //codec.invertADC(0x3F);
   //delay(200); // wait for dc offset to stabilize
   //codec.filterFreeze();

   //myTimer.begin(print_audio_usage, 500000);
}

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

void loop()
{
   update();
}