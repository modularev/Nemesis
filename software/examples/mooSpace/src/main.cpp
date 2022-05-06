#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <SerialFlash.h>
#include <elapsedMillis.h>
#include <utility/imxrt_hw.h>
#include "nemesis.h"
#include "mooSpace.h"

#include <ADC.h>
#include <ADC_util.h>

#define ADC_AVERAGING 16
#define ADC_RES 10


ADC *adc = new ADC();


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
    "modulate", // POT B
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

//elapsedMicros hysteresis[9];

// 
// 
// 
extern unsigned long _heap_end;
extern char *__brkval;
uint32_t FreeMem(){ // for Teensy 4.1
  char* p = (char*) extmem_malloc(100000); // size should be quite big, to avoid allocating fragment!
  extmem_free(p);
  return (char *)&_heap_end - p; // __brkval;
}

void print_audio_usage()
{
   // Serial.print(FreeMem());
   // Serial.print("\t");
   Serial.print(faustObj.processorUsage());
   Serial.print("\t");
   Serial.println(AudioMemoryUsageMax());
}

#define SMOO_FACTOR 0.1
#define WINDOW_SIZE 16
#define INT12_DIV 2.44200244200244e-4


   float pot_val[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};
   float pot_smoo[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};
   uint16_t pot_raw[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};
   int pot_sum[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};
   int pot_readings[9][WINDOW_SIZE];
   int pot_index[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};
   int current_pot = 0;

void send_params(int i)
{ // !!!RANGE IS INVERTED!!!
   // fValue[i] = -(float)((adc_new[i]>>2) - nemesis::getADC_min(i)) / (float)(nemesis::getADC_min(i) - nemesis::getADC_max(i));
   // if (fValue[i] > 0.994) 
   //    {
   //       fValue[i] = 1.0;
   //    }
   // else if (fValue[i] < 0.006) 
   //    {
   //       fValue[i] = 0.0;
   //    }
   faustObj.setParamValue(param[i], pot_val[i]);//clamp(fValue[i]));
}

void update()
{
   // for (int i = 0; i < 9; i++)   
   // {
   //    adc_new[i] = adc_new[i] + smoothing * (adc->analogRead(analog_pin[i]) - adc_new[i]);
   //    //if (abs(adc_new[i] - adc_old[i]) > 5 || hysteresis[i] < 100)
   //    //{
   //       adc_old[i] = adc_new[i];
   //       adc_new[i] = adc_new[i];
   //       send_params(i);
   //       //hysteresis[i] = 0;
   //       //Serial.print(i);
   //       //Serial.print("\t");
   //       //Serial.println(fValue[i], 3);
   //    //}
   // }

      pot_sum[current_pot] = pot_sum[current_pot] - pot_readings[current_pot][pot_index[current_pot]];
      pot_raw[current_pot] = 4095 - ((adc->analogRead(analog_pin[current_pot]))<<2)   ; // * INT9_DIV;      // read sensor value
      pot_readings[current_pot][pot_index[current_pot]] = pot_raw[current_pot];
      pot_sum[current_pot] = pot_sum[current_pot] + pot_readings[current_pot][pot_index[current_pot]];

      pot_index[current_pot]++;
      if (pot_index[current_pot] >= WINDOW_SIZE)
         pot_index[current_pot] = 0;

      pot_smoo[current_pot] = ((float)pot_sum[current_pot] / WINDOW_SIZE);
      pot_val[current_pot] = SMOO_FACTOR * (pot_smoo[current_pot] * INT12_DIV) + (1. - SMOO_FACTOR) * pot_val[current_pot];
      send_params(current_pot);
      current_pot++;
      if (current_pot >= 9)
         current_pot = 0;
}

void setup()
{
//    Serial.begin(9600);
//    while (!Serial) {
//     ; // wait for serial port to connect. Needed for native USB
//   }
   // Serial.print(CrashReport);
   
   nemesis::init();

   for (int i = 0; i < 9; i++)
   {
      pinMode(analog_pin[i], INPUT);
   }
      // Setup both adc modules on teensy
   adc->adc0->setAveraging(ADC_AVERAGING);                          // set number of averages
   adc->adc0->setResolution(ADC_RES);                               // set bits of resolution
   // adc->adc0->setConversionSpeed(ADC_CONVERSION_SPEED::LOW_SPEED);  // change the conversion speed
   // adc->adc0->setSamplingSpeed(ADC_SAMPLING_SPEED::MED_SPEED); // change the sampling speed
   // adc->adc0->recalibrate();

   adc->adc1->setAveraging(ADC_AVERAGING);                          // set number of averages
   adc->adc1->setResolution(ADC_RES);                               // set bits of resolution
   // adc->adc1->setConversionSpeed(ADC_CONVERSION_SPEED::LOW_SPEED);  // change the conversion speed
   // adc->adc1->setSamplingSpeed(ADC_SAMPLING_SPEED::MED_SPEED); // change the sampling speed
   // adc->adc1->recalibrate();
   
   //nemesis::setSampleRate((int)AUDIO_SAMPLE_RATE_EXACT);
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
   // analogReadAveraging(4);
   //codec.invertDAC(0x3F);
   //codec.invertADC(0x3F);
   //delay(200); // wait for dc offset to stabilize
   //codec.filterFreeze();
   //print_audio_usage();
   //myTimer.begin(print_audio_usage, 500000);
   nemesis::codec.volume(1.0);

}

void loop()
{
   update();
}