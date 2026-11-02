//
// Library for the programmable Eurorack DSP Module NEMESIS
//
// nemesis_rev2.cpp
//
// Hardware revision 2: CV X Y Z A B C D on an LTC1867 SPI ADC, pots A-D on
// Teensy analog pins, a 128x32 SSD1306 OLED, two push buttons with LEDs and
// an encoder with push switch.
//
// Knob calibration (POT A-D): hold both buttons for 2 seconds while running
// (or button 1 while powering up), turn the knobs through their full range,
// then press button 2 to store (button 1 cancels).
//
// Volt calibration (CV inputs and outputs) is measured externally, e.g. with
// tests/cvtest, and set with setCvCalibration()/setOutputCalibration().
// Calibrated CV inputs read 0..1 = 0..8 V (A-D) and -4..+4 V (X/Y/Z, the
// main knob adds an offset), outputs give 5 V per unit.
//
// Copyright © 2021 Arev Imer if not mentioned elsewise
// released under GNU GPL v3
//

#include "nemesis.h"

#if NEMESIS_HW_REV == 2

#include "nemesis_hw.h"
#include <ADC.h>
#include <EEPROM.h>
#include <Encoder.h>
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

         // Pots are converted in the background: the ADC conversion complete
         // interrupt reads a pot, filters it and starts the next one. Measured on
         // rev2 with codec and display running (12 bit, pots centred): a single
         // reading has 24-33 LSB noise (sd), 32x averaging at medium speed 1.0-1.3
         // LSB in 66us, the best noise per time of all tested settings.
         constexpr int pot_pin[POT_NUM] = {A14, A15, A16, A17};
         constexpr int ADC_AVERAGING = 32;
         constexpr int ADC_RES = 12;
         constexpr float ADC_MAX = 4095.0f;
         constexpr uint8_t ADC_ISR_PRIORITY = 192; // below audio

         // adaptive one pole filter: slow while a knob rests (removes the
         // remaining noise), fast while it is turned (no lag)
         constexpr float POT_TAU_REST = 0.02f; // seconds
         constexpr float POT_SNAP_LSB = 24.0f; // difference for full speed, ~20x the noise

         constexpr int led_pin[NUM_LEDS] = {33, 34};
         constexpr int switch_pin[NUM_BUTTONS] = {35, 36};
         constexpr int CODEC_ENABLE_PIN = 9;
         constexpr int ENC_A_PIN = 4;
         constexpr int ENC_B_PIN = 5;
         constexpr int ENC_SW_PIN = 6;

         constexpr uint32_t DISPLAY_INTERVAL_MS = 100;

         ADC adc;

         float cv_val[CV_NUM];
         int next_cv = 0;
         elapsedMicros since_conversion;

         volatile float pot_val[POT_NUM]; // 0..1, written in the ADC interrupts

         // pots handled by one ADC module, converted round robin
         struct PotScanner
         {
            ADC_Module *module = nullptr;
            int pots[POT_NUM];
            int count = 0;
            int current = 0;
            float filtered[POT_NUM]; // in LSB
            float rest_coeff = 0.0f;
            elapsedMicros since_start;
         };
         PotScanner scanners[2];

         void potConverted(PotScanner &s)
         {
            int pot = s.pots[s.current];
            float x = s.module->readSingle();
            float d = x - s.filtered[s.current];
            float snap = min(fabsf(d) / POT_SNAP_LSB, 1.0f);
            float a = s.rest_coeff + (1.0f - s.rest_coeff) * snap * snap;
            s.filtered[s.current] += a * d;
            pot_val[pot] = s.filtered[s.current] / ADC_MAX;

            s.current = (s.current + 1) % s.count;
            s.module->startSingleRead(pot_pin[s.pots[s.current]]);
         }

         void adc0Isr() { potConverted(scanners[0]); }
         void adc1Isr() { potConverted(scanners[1]); }

         bool monitor = true;
         elapsedMillis since_display;

         constexpr uint32_t CALIBRATION_HOLD_MS = 2000;
         elapsedMillis both_buttons_held;

         Encoder encoder(ENC_A_PIN, ENC_B_PIN);

         // knob range per input, stored in EEPROM
         struct Calibration
         {
            uint32_t magic;
            float lo[NUM_INPUTS];
            float hi[NUM_INPUTS];
         };
         constexpr uint32_t CALIBRATION_MAGIC = 0x4E43A202; // "NC", rev2, version 2
         constexpr int CALIBRATION_ADDRESS = 0;
         constexpr float MIN_SPAN = 0.1f; // inputs moved less than this stay uncalibrated
         constexpr float END_MARGIN = 0.005f; // so both ends are reached despite noise
         Calibration calibration;

         // CV inputs: raw reading at 0 V and per volt; outputs: volts per unit and at 0
         struct VoltCalibration
         {
            uint32_t magic;
            float cv_zero[CV_NUM];
            float cv_per_volt[CV_NUM];
            float out_per_unit[NUM_OUTPUTS];
            float out_offset[NUM_OUTPUTS];
         };
         constexpr uint32_t VOLT_CALIBRATION_MAGIC = 0x4E56A201; // "NV", rev2, version 1
         constexpr int VOLT_CALIBRATION_ADDRESS = 128;
         constexpr int CV_BIPOLAR = 3; // CV X Y Z are bipolar
         constexpr float CV_SPAN_VOLTS = 8.0f;
         constexpr float OUTPUT_VOLTS = 5.0f; // per unit
         VoltCalibration volts;

         float raw(int index)
         {
            return index < CV_NUM ? cv_val[index] : pot_val[index - CV_NUM];
         }

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

         FLASHMEM void beginPots()
         {
            ADC_Module *modules[2] = {adc.adc0, adc.adc1};
            for (ADC_Module *m : modules)
            {
               m->setAveraging(ADC_AVERAGING);
               m->setResolution(ADC_RES);
               m->setConversionSpeed(ADC_CONVERSION_SPEED::MED_SPEED);
               m->setSamplingSpeed(ADC_SAMPLING_SPEED::MED_SPEED);
            }

            for (int i = 0; i < POT_NUM; i++)
            {
               pinMode(pot_pin[i], INPUT_DISABLE);
               int n = adc.adc0->checkPin(pot_pin[i]) ? 0 : 1;
               PotScanner &s = scanners[n];
               s.module = modules[n];
               s.filtered[s.count] = s.module->analogRead(pot_pin[i]);
               pot_val[i] = s.filtered[s.count] / ADC_MAX;
               s.pots[s.count++] = i;
            }

            for (int n = 0; n < 2; n++)
            {
               PotScanner &s = scanners[n];
               if (s.count == 0)
                  continue;
               // one conversion takes ~66us, every pot of this module is sampled
               // count times slower; measured below to set the filter coefficient
               s.since_start = 0;
               for (int i = 0; i < 20 * s.count; i++)
                  s.module->analogRead(pot_pin[s.pots[i % s.count]]);
               float sample_period = s.since_start * 1e-6f / 20.0f;
               s.rest_coeff = 1.0f - expf(-sample_period / POT_TAU_REST);

               s.module->enableInterrupts(n == 0 ? adc0Isr : adc1Isr, ADC_ISR_PRIORITY);
               s.module->startSingleRead(pot_pin[s.pots[0]]);
            }
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

         // design values: 14 bit ADC over 8.33 V, X/Y/Z centred, +-5 V outputs
         FLASHMEM void resetVolts()
         {
            volts.magic = VOLT_CALIBRATION_MAGIC;
            for (int i = 0; i < CV_NUM; i++)
            {
               volts.cv_zero[i] = i < CV_BIPOLAR ? 0.5f : 0.0f;
               volts.cv_per_volt[i] = 0.12f;
            }
            for (int i = 0; i < NUM_OUTPUTS; i++)
            {
               volts.out_per_unit[i] = OUTPUT_VOLTS;
               volts.out_offset[i] = 0.0f;
            }
         }

         FLASHMEM void resetCalibration()
         {
            calibration.magic = CALIBRATION_MAGIC;
            for (int i = 0; i < NUM_INPUTS; i++)
            {
               calibration.lo[i] = 0.0f;
               calibration.hi[i] = 1.0f;
            }
         }

         FLASHMEM void loadCalibration()
         {
            EEPROM.get(CALIBRATION_ADDRESS, calibration);
            if (calibration.magic != CALIBRATION_MAGIC)
               resetCalibration();

            EEPROM.get(VOLT_CALIBRATION_ADDRESS, volts);
            if (volts.magic != VOLT_CALIBRATION_MAGIC)
               resetVoltCalibration();
         }

         FLASHMEM void waitForRelease()
         {
            while (buttonPressed(0) || buttonPressed(1))
               scan();
            delay(50);
         }

         // spans of the pots in percent
         FLASHMEM void drawCalibration(const float *lo, const float *hi)
         {
            display.clearDisplay();
            display.setFont(&SourceCodePro_ExtraLight6pt7b); // a sketch may have changed it
            display.setTextColor(WHITE);
            display.setCursor(0, 7);
            display.print(F("CAL  B2 save  B1 cancel"));
            for (int p = 0; p < POT_NUM; p++)
            {
               int i = CV_NUM + p;
               display.setCursor(p * 32, 25);
               display.printf("%c %d", 'A' + p, hi[i] > lo[i] ? (int)((hi[i] - lo[i]) * 99.0f + 0.5f) : 0);
            }
            display.display();
         }

         FLASHMEM void calibrate()
         {
            float lo[NUM_INPUTS], hi[NUM_INPUTS];
            for (int i = 0; i < NUM_INPUTS; i++)
            {
               lo[i] = 1.0f;
               hi[i] = 0.0f;
            }

            setLed(0, true);
            Serial.println("Calibration: turn POT A-D through their full range, "
                           "then press button 2 to store, button 1 to cancel");
            waitForRelease();

            elapsedMillis since_draw;
            elapsedMillis since_start;
            bool store = false;
            while (true)
            {
               scan();
               if (since_start > 300) // let the smoothing settle first
               {
                  for (int i = CV_NUM; i < NUM_INPUTS; i++)
                  {
                     lo[i] = min(lo[i], raw(i));
                     hi[i] = max(hi[i], raw(i));
                  }
               }
               if (buttonPressed(1))
               {
                  store = true;
                  break;
               }
               if (buttonPressed(0))
                  break;
               if (since_draw >= 100)
               {
                  since_draw = 0;
                  drawCalibration(lo, hi);
               }
            }

            if (store)
            {
               resetCalibration();
               for (int i = CV_NUM; i < NUM_INPUTS; i++)
               {
                  if (hi[i] - lo[i] >= MIN_SPAN)
                  {
                     calibration.lo[i] = lo[i] + END_MARGIN;
                     calibration.hi[i] = hi[i] - END_MARGIN;
                  }
                  Serial.printf("%-6s %.3f .. %.3f%s\n", inputNames[i], calibration.lo[i], calibration.hi[i],
                                hi[i] - lo[i] >= MIN_SPAN ? "" : "  (not moved, uncalibrated)");
               }
               EEPROM.put(CALIBRATION_ADDRESS, calibration);
               Serial.println("Calibration stored");
            }
            else
            {
               Serial.println("Calibration cancelled");
            }

            display.clearDisplay();
            display.setFont(&SourceCodePro_ExtraLight6pt7b);
            display.setCursor(0, 19);
            display.print(store ? F("calibration stored") : F("calibration cancelled"));
            display.display();
            setLed(0, false);
            waitForRelease();
            delay(700);
         }

         // input values: CV X Y Z / A B C / D, POT A / B C D
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
         pinMode(ENC_SW_PIN, INPUT_PULLUP);
         pinMode(CODEC_ENABLE_PIN, OUTPUT);

         display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
         splash(program.name());

         pinMode(PIN_SPI_SS, OUTPUT);
         digitalWriteFast(PIN_SPI_SS, HIGH);
         SPI.begin();
         transferLTC(next_cv); // dummy transfer to start the first conversion

         beginPots();

         loadCalibration();
         if (buttonPressed(0))
            calibrate();

         digitalWrite(CODEC_ENABLE_PIN, HIGH);
         AudioMemory(NEMESIS_AUDIO_MEMORY);
         codec.enable();
         codecWrite(0x03, 0xF4 | 0x01); // functional mode
         codecWrite(0x05, 0x00);        // ADC control
         codec.invertDAC(0xFF);         // the output stages invert, measured on rev2
         codec.volume(1.0);
      }

      // pots are read in the ADC interrupts
      void scan()
      {
         scanCV();
      }

      void service(const Program &program)
      {
         if (!(buttonPressed(0) && buttonPressed(1)))
            both_buttons_held = 0;
         else if (both_buttons_held >= CALIBRATION_HOLD_MS)
            calibrate();

         if (monitor && since_display >= DISPLAY_INTERVAL_MS)
         {
            since_display = 0;
            drawMonitor();
         }
      }

      float cvVolts(int index)
      {
         return (raw(index) - volts.cv_zero[index]) / volts.cv_per_volt[index];
      }

      float input(int index)
      {
         float v;
         if (index < CV_NUM)
            v = (cvVolts(index) + (index < CV_BIPOLAR ? CV_SPAN_VOLTS / 2 : 0.0f)) / CV_SPAN_VOLTS;
         else
            v = (raw(index) - calibration.lo[index]) / (calibration.hi[index] - calibration.lo[index]);
         return constrain(v, 0.0f, 1.0f);
      }

      FLASHMEM void resetVoltCalibration() { resetVolts(); }

      float rawInput(int index) { return raw(index); }

      int aliasIndex(const char *name) { return -1; }

      // LEDs in the push buttons are active high (checked on hardware)
      void setLed(int index, bool on) { digitalWrite(led_pin[index], on ? HIGH : LOW); }

      bool buttonPressed(int index) { return digitalRead(switch_pin[index]) == LOW; }
   }

   void showMonitor(bool enable) { hw::monitor = enable; }

   float cvVolts(int index)
   {
      return (index >= 0 && index < hw::CV_NUM) ? hw::cvVolts(index) : 0.0f;
   }

   FLASHMEM void setCvCalibration(int index, float zeroReading, float readingPerVolt)
   {
      if (index < 0 || index >= hw::CV_NUM || readingPerVolt <= 0.0f)
         return;
      hw::volts.cv_zero[index] = zeroReading;
      hw::volts.cv_per_volt[index] = readingPerVolt;
   }

   FLASHMEM void setOutputCalibration(int output, float voltsPerUnit, float offsetVolts)
   {
      if (output < 0 || output >= NUM_OUTPUTS || voltsPerUnit <= 0.0f)
         return;
      hw::volts.out_per_unit[output] = voltsPerUnit;
      hw::volts.out_offset[output] = offsetVolts;
   }

   FLASHMEM void storeVoltCalibration() { EEPROM.put(hw::VOLT_CALIBRATION_ADDRESS, hw::volts); }

   FLASHMEM void resetVoltCalibration() { hw::resetVolts(); }

   FLASHMEM void printVoltCalibration(Stream &out)
   {
      for (int i = 0; i < hw::CV_NUM; i++)
         out.printf("%s zero %.5f per volt %.5f\n", hw::inputNames[i], hw::volts.cv_zero[i], hw::volts.cv_per_volt[i]);
      for (int i = 0; i < NUM_OUTPUTS; i++)
         out.printf("OUT%d %.4f V per unit, %+.4f V at 0\n", i + 1, hw::volts.out_per_unit[i], hw::volts.out_offset[i]);
   }

   // applied by the architecture file to every output sample: corrected = sample * scale + offset
   void outputCorrection(int output, float &scale, float &offset)
   {
      if (output < 0 || output >= NUM_OUTPUTS)
      {
         scale = 1.0f;
         offset = 0.0f;
         return;
      }
      scale = hw::OUTPUT_VOLTS / hw::volts.out_per_unit[output];
      offset = -hw::volts.out_offset[output] / hw::volts.out_per_unit[output];
   }

   // clockwise counts down with A on pin 4 and B on pin 5, hence the sign
   int32_t encoderPosition() { return -hw::encoder.read() / NEMESIS_ENCODER_COUNTS_PER_STEP; }

   bool encoderPressed() { return digitalRead(hw::ENC_SW_PIN) == LOW; }
}

#endif
