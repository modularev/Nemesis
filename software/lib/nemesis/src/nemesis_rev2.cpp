//
// Library for the programmable Eurorack DSP Module NEMESIS
//
// nemesis_rev2.cpp
//
// Hardware revision 2: CV X Y Z A B C D on an LTC1867 SPI ADC, pots A-D on
// Teensy analog pins and a 128x32 SSD1306 OLED.
//
// Copyright © 2021 Arev Imer if not mentioned elsewise
// released under GNU GPL v3
//

#include "nemesis.h"

#if NEMESIS_HW_REV == 2

#include "nemesis_hw.h"
#include <ADC.h>
#include <SPI.h>
#include <Wire.h>
#include "fonts/SourceCodePro6.h"
#include "fonts/SourceCodeProItalic14.h"

namespace nemesis
{
   Adafruit_SSD1306 display(128, 32, &Wire, -1);

   namespace hw
   {
      const char *const inputNames[NUM_INPUTS] = {
          "CV_X", "CV_Y", "CV_Z", "CV_A", "CV_B", "CV_C", "CV_D",
          "POT_A", "POT_B", "POT_C", "POT_D"};

      namespace
      {
         constexpr int CV_NUM = 7;
         constexpr int POT_NUM = 4;

         // LTC1867 single ended unipolar channel commands, last two channels swapped on v0.2
#define CW(N) (0x80 + (N << 4) + 0x0C)
         constexpr uint8_t adc_chan_word[CV_NUM] = {CW(0), CW(4), CW(1), CW(5), CW(2), CW(3), CW(6)};
#undef CW
         // LTC1867L datasheet: SCK max 10 MHz when SDO is captured on the rising
         // edge (mode 0, which also meets the SDI hold time), conversion up to
         // 3.7us plus at least 2.01us acquisition before the next conversion
         const SPISettings LTC_SPI(10000000, MSBFIRST, SPI_MODE0);
         constexpr uint32_t LTC_CYCLE_US = 8;
         constexpr float LTC_SMOO = 0.2f;
         constexpr float INT14_DIV = 1.0f / 16383.0f;

         constexpr int pot_pin[POT_NUM] = {A14, A15, A16, A17};
         constexpr int ADC_AVERAGING = 16;
         constexpr int ADC_RES = 12;
         constexpr float INT12_DIV = 1.0f / 4095.0f;
         constexpr int POT_THRESHOLD = 20;
         constexpr int WINDOW_SIZE = 16;
         constexpr float SMOO_FACTOR = 0.8f;

         constexpr int led_pin[NUM_LEDS] = {33, 34};
         constexpr int switch_pin[NUM_BUTTONS] = {35, 36};
         constexpr int CODEC_ENABLE_PIN = 9;

         constexpr uint32_t DISPLAY_INTERVAL_MS = 100;

         ADC adc;

         float cv_val[CV_NUM];
         int next_cv = 0;
         elapsedMicros since_conversion;

         float pot_val[POT_NUM];
         int pot_readings[POT_NUM][WINDOW_SIZE];
         int pot_sum[POT_NUM];
         int pot_index[POT_NUM];
         int current_pot = 0;

         bool monitor = true;
         elapsedMillis since_display;

         // read the conversion started by the previous call and start the next one
         uint16_t transferLTC(int next_chan)
         {
            SPI.beginTransaction(LTC_SPI);
            digitalWriteFast(PIN_SPI_SS, LOW);
            uint16_t raw = SPI.transfer16(adc_chan_word[next_chan] << 8);
            digitalWriteFast(PIN_SPI_SS, HIGH); // rising edge starts the conversion
            SPI.endTransaction();
            since_conversion = 0;
            return raw;
         }

         // the opamp in front of the ADC inverts the reading, 14 of 16 bit are used
         float scaleLTC(uint16_t raw)
         {
            return ((0x3FFF - (raw >> 2)) & 0x3FFF) * INT14_DIV;
         }

         void scanCV()
         {
            if (since_conversion < LTC_CYCLE_US)
               return;
            int chan = next_cv;
            next_cv = (next_cv + 1) % CV_NUM;
            cv_val[chan] = LTC_SMOO * scaleLTC(transferLTC(next_cv)) + (1.0f - LTC_SMOO) * cv_val[chan];
         }

         int readPot(int i)
         {
            int raw = adc.analogRead(pot_pin[i]);
            return raw > POT_THRESHOLD ? raw : 0;
         }

         void scanPot()
         {
            int i = current_pot;
            int raw = readPot(i);
            pot_sum[i] += raw - pot_readings[i][pot_index[i]];
            pot_readings[i][pot_index[i]] = raw;
            pot_index[i] = (pot_index[i] + 1) % WINDOW_SIZE;

            pot_val[i] = SMOO_FACTOR * ((pot_sum[i] / WINDOW_SIZE) * INT12_DIV) + (1.0f - SMOO_FACTOR) * pot_val[i];
            current_pot = (current_pot + 1) % POT_NUM;
         }

         FLASHMEM void splash(const char *programName)
         {
            display.clearDisplay();
            display.setTextSize(1);
            display.setTextColor(WHITE);
            display.setFont(&SourceCodePro_ExtraLight6pt7b);
            display.setCursor(0, 7);
            display.print(F("say hello to"));
            display.setCursor(0, 31);
            display.setFont(&SourceCodePro_ExtraLightItalic14pt7b);
            display.print(F(">NEMESIS"));
            display.display();
            delay(1500);

            display.clearDisplay();
            display.setFont(&SourceCodePro_ExtraLight6pt7b);
            display.setCursor(0, 7);
            display.print(F("running"));
            display.setCursor(0, 31);
            display.setFont(&SourceCodePro_ExtraLightItalic14pt7b);
            display.print(programName);
            display.display();
            display.setFont(&SourceCodePro_ExtraLight6pt7b);
            delay(1000);
         }

         // raw input values: CV X Y Z / A B C / D, POT A / B C D
         FLASHMEM void drawMonitor()
         {
            static const uint8_t pos[NUM_INPUTS][2] = {
                {0, 7}, {0, 17}, {0, 27}, {32, 7}, {32, 17}, {32, 27}, {64, 7}, {64, 27}, {96, 7}, {96, 17}, {96, 27}};
            display.clearDisplay();
            for (int i = 0; i < NUM_INPUTS; i++)
            {
               display.setCursor(pos[i][0], pos[i][1]);
               display.print(input(i), 2);
            }
            display.display();
         }
      }

      FLASHMEM void setPSRAMspeed()
      {
         CCM_CCGR7 |= CCM_CCGR7_FLEXSPI2(CCM_CCGR_OFF);
         CCM_CBCMR = (CCM_CBCMR & ~(CCM_CBCMR_FLEXSPI2_PODF_MASK | CCM_CBCMR_FLEXSPI2_CLK_SEL_MASK)) | CCM_CBCMR_FLEXSPI2_PODF(3) // 4 and 2 133MHz
                     | CCM_CBCMR_FLEXSPI2_CLK_SEL(1);
         CCM_CCGR7 |= CCM_CCGR7_FLEXSPI2(CCM_CCGR_ON);
      }

      FLASHMEM void begin(const Program &program)
      {
         setPSRAMspeed();

         for (int i = 0; i < NUM_LEDS; i++)
         {
            pinMode(led_pin[i], OUTPUT);
            setLed(i, false);
         }
         for (int i = 0; i < NUM_BUTTONS; i++)
            pinMode(switch_pin[i], INPUT);
         pinMode(CODEC_ENABLE_PIN, OUTPUT);

         display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
         splash(program.name());

         for (int i = 0; i < POT_NUM; i++)
            pinMode(pot_pin[i], INPUT_DISABLE);
         adc.adc0->setAveraging(ADC_AVERAGING);
         adc.adc0->setResolution(ADC_RES);
         adc.adc0->setConversionSpeed(ADC_CONVERSION_SPEED::MED_SPEED);
         adc.adc0->setSamplingSpeed(ADC_SAMPLING_SPEED::VERY_LOW_SPEED);
         adc.adc1->setAveraging(ADC_AVERAGING);
         adc.adc1->setResolution(ADC_RES);
         adc.adc1->setConversionSpeed(ADC_CONVERSION_SPEED::MED_SPEED);
         adc.adc1->setSamplingSpeed(ADC_SAMPLING_SPEED::VERY_LOW_SPEED);

         pinMode(PIN_SPI_SS, OUTPUT);
         digitalWriteFast(PIN_SPI_SS, HIGH);
         SPI.begin();
         transferLTC(next_cv); // dummy transfer to start the first conversion

         // prefill the moving averages so the pots start at their position
         for (int i = 0; i < POT_NUM; i++)
         {
            int raw = readPot(i);
            for (int n = 0; n < WINDOW_SIZE; n++)
               pot_readings[i][n] = raw;
            pot_sum[i] = raw * WINDOW_SIZE;
            pot_val[i] = raw * INT12_DIV;
         }

         digitalWrite(CODEC_ENABLE_PIN, HIGH);
         AudioMemory(NEMESIS_AUDIO_MEMORY);
         codec.enable();
         codecWrite(0x03, 0xF4 | 0x01); // functional mode
         codecWrite(0x05, 0x00);        // ADC control
         codec.invertDAC(0xFF);         // the output stages invert, measured on rev2
         codec.volume(1.0);
      }

      void scan()
      {
         scanPot();
         scanCV();
      }

      void service(const Program &program)
      {
         if (monitor && since_display >= DISPLAY_INTERVAL_MS)
         {
            since_display = 0;
            drawMonitor();
         }
      }

      float input(int index)
      {
         return index < CV_NUM ? cv_val[index] : pot_val[index - CV_NUM];
      }

      int aliasIndex(const char *name) { return -1; }

      // LEDs in the push buttons are active high (checked on hardware)
      void setLed(int index, bool on) { digitalWrite(led_pin[index], on ? HIGH : LOW); }

      bool buttonPressed(int index) { return digitalRead(switch_pin[index]) == LOW; }
   }

   void showMonitor(bool enable) { hw::monitor = enable; }
}

#endif
