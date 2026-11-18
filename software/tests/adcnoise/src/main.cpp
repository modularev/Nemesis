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

bool begun = false; // the watchdog only runs after nemesis::begin()

void measureAll(const char *title)
{
   Serial.printf("--- %s\n", title);
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
         if (begun)
            nemesis::watchdogFeed();
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
}

void setup()
{
   // first without the library: no display, codec, CV scanning, pot
   // interrupts or watchdog (2026-11-17: is the pot noise from them?)
   while (!Serial && millis() < 10000)
   {
   }
   for (int i = 0; i < 4; i++)
      pinMode(POTS[i], INPUT_DISABLE);
   measureAll("before nemesis::begin(): Teensy alone");

   nemesis::begin(passthrough);
   begun = true;
   nemesis::showMonitor(false);
   nemesis::display.clearDisplay();
   nemesis::display.setCursor(0, 19);
   nemesis::display.print("ADC test - hands off");
   nemesis::display.display();
   delay(1000);
   nemesis::watchdogFeed();
   measureAll("after nemesis::begin(): display, codec, CV scan running");

   // softer edges on the audio pins: drive strength and slew rate
   Serial.printf("pad config before: MCLK 23 %08lx BCLK 21 %08lx LRCLK 20 %08lx TX 7 %08lx RX 8 %08lx\n",
                 CORE_PIN23_PADCONFIG, CORE_PIN21_PADCONFIG, CORE_PIN20_PADCONFIG, CORE_PIN7_PADCONFIG, CORE_PIN8_PADCONFIG);
   uint32_t keep = CORE_PIN23_PADCONFIG & ~(IOMUXC_PAD_DSE(7) | IOMUXC_PAD_SPEED(3) | IOMUXC_PAD_SRE);
   for (int dse : {3, 1})
   {
      uint32_t pad = keep | IOMUXC_PAD_DSE(dse) | IOMUXC_PAD_SPEED(0); // SRE 0: slow slew
      CORE_PIN23_PADCONFIG = pad;
      CORE_PIN21_PADCONFIG = pad;
      CORE_PIN20_PADCONFIG = pad;
      CORE_PIN7_PADCONFIG = pad;
      CORE_PIN8_PADCONFIG = pad;
      delay(50);
      nemesis::watchdogFeed();
      char title[64];
      snprintf(title, sizeof(title), "+ audio pins slow slew, drive strength %d", dse);
      measureAll(title);
   }

   // the audio interface: SAI1 transmit/receive and bit clocks off, MCLK
   // (pin 23) no longer driven to the codec
   I2S1_TCSR &= ~(I2S_TCSR_TE | I2S_TCSR_BCE);
   I2S1_RCSR &= ~(I2S_RCSR_RE | I2S_RCSR_BCE);
   pinMode(23, OUTPUT);
   digitalWriteFast(23, LOW);
   delay(100);
   nemesis::watchdogFeed();
   measureAll("+ SAI1 and MCLK to the codec stopped");

   nemesis::display.ssd1306_command(0x8D);
   nemesis::display.ssd1306_command(0x14);
   nemesis::display.ssd1306_command(0xAF);
   Serial.println("done");
   nemesis::display.clearDisplay();
   nemesis::display.setCursor(0, 19);
   nemesis::display.print("ADC test done");
   nemesis::display.display();
}

void loop() { nemesis::watchdogFeed(); }
