//
// ADC noise measurement for the NEMESIS rev2 pots
//
// Runs with codec and display active (like a real program), then reads each
// pot many times per ADC configuration. Leave the pots untouched.
// Prints noise (std dev, peak-peak in 12 bit LSB) and time per reading.
//

#include <nemesis.h>
#include <ADC.h>
#include "hwtest.h"

hwtest passthrough;
ADC adc;

constexpr int POTS[4] = {A14, A15, A16, A17};
constexpr int SAMPLES = 2000;

struct Config
{
   const char *name;
   int averaging;
   ADC_CONVERSION_SPEED conversion;
   ADC_SAMPLING_SPEED sampling;
};

const Config configs[] = {
    {"avg1  med/med     ", 1, ADC_CONVERSION_SPEED::MED_SPEED, ADC_SAMPLING_SPEED::MED_SPEED},
    {"avg4  med/med     ", 4, ADC_CONVERSION_SPEED::MED_SPEED, ADC_SAMPLING_SPEED::MED_SPEED},
    {"avg16 med/med     ", 16, ADC_CONVERSION_SPEED::MED_SPEED, ADC_SAMPLING_SPEED::MED_SPEED},
    {"avg32 med/med     ", 32, ADC_CONVERSION_SPEED::MED_SPEED, ADC_SAMPLING_SPEED::MED_SPEED},
    {"avg16 med/verylow ", 16, ADC_CONVERSION_SPEED::MED_SPEED, ADC_SAMPLING_SPEED::VERY_LOW_SPEED},
    {"avg32 low/verylow ", 32, ADC_CONVERSION_SPEED::LOW_SPEED, ADC_SAMPLING_SPEED::VERY_LOW_SPEED},
    {"avg4  low/verylow ", 4, ADC_CONVERSION_SPEED::LOW_SPEED, ADC_SAMPLING_SPEED::VERY_LOW_SPEED},
    {"avg4  high/high   ", 4, ADC_CONVERSION_SPEED::HIGH_SPEED, ADC_SAMPLING_SPEED::HIGH_SPEED},
};

void configure(const Config &c)
{
   for (ADC_Module *m : {adc.adc0, adc.adc1})
   {
      m->setResolution(12);
      m->setAveraging(c.averaging);
      m->setConversionSpeed(c.conversion);
      m->setSamplingSpeed(c.sampling);
   }
}

void setup()
{
   nemesis::begin(passthrough);
   nemesis::showMonitor(false);
   nemesis::display.clearDisplay();
   nemesis::display.setCursor(0, 19);
   nemesis::display.print("ADC test - hands off");
   nemesis::display.display();
   delay(1000);

   while (!Serial && millis() < 10000)
   {
   }
   static uint16_t buf[SAMPLES];
   for (const Config &c : configs)
   {
      configure(c);
      Serial.printf("%s", c.name);
      for (int p = 0; p < 4; p++)
      {
         for (int i = 0; i < 50; i++)
            adc.analogRead(POTS[p]); // settle
         elapsedMicros t;
         for (int i = 0; i < SAMPLES; i++)
            buf[i] = adc.analogRead(POTS[p]);
         float us = (float)t / SAMPLES;
         double sum = 0, sq = 0;
         int lo = 4095, hi = 0;
         for (int i = 0; i < SAMPLES; i++)
         {
            sum += buf[i];
            lo = min(lo, (int)buf[i]);
            hi = max(hi, (int)buf[i]);
         }
         double mean = sum / SAMPLES;
         for (int i = 0; i < SAMPLES; i++)
            sq += (buf[i] - mean) * (buf[i] - mean);
         Serial.printf(" | P%d mean %6.1f sd %5.2f pp %3d %5.1fus", p, mean, sqrt(sq / SAMPLES), hi - lo, us);
      }
      Serial.println();
   }
   Serial.println("done");
   nemesis::display.clearDisplay();
   nemesis::display.setCursor(0, 19);
   nemesis::display.print("ADC test done");
   nemesis::display.display();
}

void loop() {}
