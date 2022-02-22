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

#ifdef HIGHRES
AudioConnection Lin_MSB(audio_in, 0, faustObj, 0);
AudioConnection Lin_LSB(audio_in, 1, faustObj, 1);
AudioConnection Rin_MSB(audio_in, 2, faustObj, 2);
AudioConnection Rin_LSB(audio_in, 3, faustObj, 3);
AudioConnection Lout_MSB(faustObj, 0, audio_out, 0);
AudioConnection Lout_LSB(faustObj, 1, audio_out, 1);
AudioConnection Rout_MSB(faustObj, 2, audio_out, 2);
AudioConnection Rout_LSB(faustObj, 3, audio_out, 3);
#else
AudioConnection Lin_MSB(audio_in, 0, faustObj, 0);
AudioConnection Rin_MSB(audio_in, 2, faustObj, 1);
AudioConnection Lout_MSB(faustObj, 0, audio_out, 0);
AudioConnection Rout_MSB(faustObj, 1, audio_out, 2);
#endif

IntervalTimer myTimer;

//
// Parameters from Faust
//
const char *param[] = {
    "smear",   // POT X
    "decay",   // POT Y
    "drag",    // POT Z
    "Filter",// POT A
    "input diffusion", // POT B
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

constexpr float smoothing = 0.2f;

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
   if (fValue[i] > 0.994) 
      {
         fValue[i] = 1.0;
      }
   else if (fValue[i] < 0.006) 
      {
         fValue[i] = 0.0;
      }
   faustObj.setParamValue(param[i], clamp(fValue[i]));
}


void setup()
{
   Serial.begin(9600);
//    while (!Serial) {
//     ; // wait for serial port to connect. Needed for native USB
//   }

   //nemesis::setSampleRate((int)AUDIO_SAMPLE_RATE_EXACT);
   nemesis::init();
   nemesis::calibration(); // calibration routine

   if (EEPROM.read(0) == 0xFF) // check for calbration flag
   {
      nemesis::loadCalibration();
      Serial.println("Calibration loaded!");
   }
   else Serial.println("No calibration found!");

   // for (int i = 0; i < 9; i++)
   // {
   //    pinMode(analog_pin[i], INPUT_DISABLE);
   // }
   
   //analogReadResolution(12);
   analogReadAveraging(4);
   //codec.invertDAC(0x3F);
   //codec.invertADC(0x3F);
   //delay(200); // wait for dc offset to stabilize
   //codec.filterFreeze();
   myTimer.begin(print_audio_usage, 500000);
   nemesis::codec.volume(1.0);

}

void update()
{
   for (int i = 0; i < 9; i++)   
   {
      adc_new[i] = adc_new[i] + smoothing * (analogRead(analog_pin[i]) - adc_new[i]);
      if (abs(adc_new[i] - adc_old[i]) > 3 || hysteresis[i] < 100)
      {
         adc_old[i] = adc_new[i];
         adc_new[i] = adc_new[i];
         send_params(i);
         hysteresis[i] = 0;
         //Serial.print(i);
         //Serial.print("\t");
         //Serial.println(fValue[i], 3);
      }
   }
}

void loop()
{
   update();
}