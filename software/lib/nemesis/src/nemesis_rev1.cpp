//
// Library for the programmable Eurorack DSP Module NEMESIS
//
// nemesis_rev1.cpp
//
// Hardware revision 1: knobs X Y Z and A-F are summed with their CV jacks
// and read on 9 Teensy analog pins (inverted range). Optional min/max
// calibration is stored in EEPROM: hold button 3 while powering up.
//
// Copyright © 2021 Arev Imer if not mentioned elsewise
// released under GNU GPL v3
//

#include "nemesis.h"

#if NEMESIS_HW_REV == 1

#include "nemesis_hw.h"
#include <ADC.h>
#include <EEPROM.h>

namespace nemesis
{
   namespace hw
   {
      const char *const inputNames[NUM_INPUTS] = {
          "CV_X", "CV_Y", "CV_Z", "CV_A", "CV_B", "CV_C", "CV_D", "CV_E", "CV_F"};

      namespace
      {
         constexpr int analog_pin[NUM_INPUTS] = {A0, A1, A2, A10, A11, A12, A13, A14, A15};
         constexpr int led_pin[NUM_LEDS] = {35, 36, 37};
         constexpr int switch_pin[NUM_BUTTONS] = {28, 29, 30};

         constexpr int ADC_AVERAGING = 16;
         constexpr int ADC_RES = 10;
         constexpr int ADC_MAX = (1 << ADC_RES) - 1;
         constexpr int WINDOW_SIZE = 16;
         constexpr float SMOO_FACTOR = 0.1f;

         // EEPROM layout of the calibration (kept from the original firmware)
         constexpr int EEPROM_FLAG = 0;
         constexpr int EEPROM_MIN = 10;
         constexpr int EEPROM_MAX = 30;

         ADC adc;

         uint16_t readings[NUM_INPUTS][WINDOW_SIZE];
         int reading_sum[NUM_INPUTS];
         int reading_index[NUM_INPUTS];
         float value[NUM_INPUTS];
         int current = 0;

         // !!!RANGE IS INVERTED!!! the reading at the knob minimum is the larger one
         uint16_t adc_min[NUM_INPUTS];
         uint16_t adc_max[NUM_INPUTS];
         bool calibrated = false;

         FLASHMEM uint16_t readAveraged(int i)
         {
            uint32_t sum = 0;
            for (int n = 0; n < 4; n++)
               sum += adc.analogRead(analog_pin[i]);
            return sum / 4;
         }

         FLASHMEM void writeIntIntoEEPROM(int address, uint16_t number)
         {
            EEPROM.write(address, number >> 8);
            EEPROM.write(address + 1, number & 0xFF);
         }

         FLASHMEM uint16_t readIntFromEEPROM(int address)
         {
            return (EEPROM.read(address) << 8) + EEPROM.read(address + 1);
         }

         FLASHMEM void waitForButton(int index)
         {
            while (digitalRead(switch_pin[index]) == HIGH)
            {
            }
            delay(50);
            while (digitalRead(switch_pin[index]) == LOW)
            {
            }
         }

         FLASHMEM void calibration()
         {
            setLed(1, true); // calibration started

            Serial.println("Turn all knobs to minimum position, then press button 1");
            waitForButton(0);
            for (int i = 0; i < NUM_INPUTS; i++)
            {
               adc_min[i] = readAveraged(i);
               writeIntIntoEEPROM(EEPROM_MIN + 2 * i, adc_min[i]);
            }

            Serial.println("Turn all knobs to maximum position, then press button 2");
            setLed(0, true); // second step
            waitForButton(1);
            for (int i = 0; i < NUM_INPUTS; i++)
            {
               adc_max[i] = readAveraged(i);
               writeIntIntoEEPROM(EEPROM_MAX + 2 * i, adc_max[i]);
            }
            EEPROM.write(EEPROM_FLAG, 0xFF);

            for (int i = 0; i < NUM_INPUTS; i++)
               Serial.printf("%s: %u .. %u\n", inputNames[i], adc_min[i], adc_max[i]);

            Serial.println("Calibration stored, press button 3 to continue");
            waitForButton(2);
            setLed(0, false);
            setLed(1, false);
         }

         FLASHMEM void loadCalibration()
         {
            if (EEPROM.read(EEPROM_FLAG) != 0xFF)
               return;
            calibrated = true;
            for (int i = 0; i < NUM_INPUTS; i++)
            {
               adc_min[i] = readIntFromEEPROM(EEPROM_MIN + 2 * i);
               adc_max[i] = readIntFromEEPROM(EEPROM_MAX + 2 * i);
               // ignore implausible data, e.g. from an aborted calibration
               if (adc_min[i] < adc_max[i] + 100)
                  calibrated = false;
            }
         }

         float scale(int i, float raw)
         {
            float v = calibrated ? (adc_min[i] - raw) / (float)(adc_min[i] - adc_max[i])
                                 : (ADC_MAX - raw) / (float)ADC_MAX;
            return constrain(v, 0.0f, 1.0f);
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
         delay(10);
         AudioMemory(NEMESIS_AUDIO_MEMORY);
         delay(10);

         for (int i = 0; i < NUM_LEDS; i++)
            pinMode(led_pin[i], OUTPUT);
         for (int i = 0; i < NUM_BUTTONS; i++)
            pinMode(switch_pin[i], INPUT);

         codec.enable();
         codecWrite(0x02, 0xFF);        // power down while configuring
         codecWrite(0x03, 0xF4 | 0x01); // functional mode
         codecWrite(0x05, 0x00);        // ADC control
         codecWrite(0x02, 0x00);        // power up
         codec.volume(1.0);

         for (int i = 0; i < NUM_INPUTS; i++)
            pinMode(analog_pin[i], INPUT);
         adc.adc0->setAveraging(ADC_AVERAGING);
         adc.adc0->setResolution(ADC_RES);
         adc.adc1->setAveraging(ADC_AVERAGING);
         adc.adc1->setResolution(ADC_RES);

         if (digitalRead(switch_pin[2]) == LOW)
            calibration();
         loadCalibration();

         // prefill the moving averages so the controls start at their position
         for (int i = 0; i < NUM_INPUTS; i++)
         {
            uint16_t raw = adc.analogRead(analog_pin[i]);
            for (int n = 0; n < WINDOW_SIZE; n++)
               readings[i][n] = raw;
            reading_sum[i] = raw * WINDOW_SIZE;
            value[i] = scale(i, raw);
         }
      }

      // one input per call, moving average plus smoothing
      void scan()
      {
         int i = current;
         uint16_t raw = adc.analogRead(analog_pin[i]);
         reading_sum[i] += raw - readings[i][reading_index[i]];
         readings[i][reading_index[i]] = raw;
         reading_index[i] = (reading_index[i] + 1) % WINDOW_SIZE;

         float averaged = reading_sum[i] / (float)WINDOW_SIZE;
         value[i] = SMOO_FACTOR * scale(i, averaged) + (1.0f - SMOO_FACTOR) * value[i];

         current = (current + 1) % NUM_INPUTS;
      }

      void service(const Program &program) {}

      float input(int index) { return value[index]; }

      // POT_X .. POT_F name the same knob+CV input as CV_X .. CV_F
      FLASHMEM int aliasIndex(const char *name)
      {
         if (strncmp(name, "POT_", 4) != 0)
            return -1;
         char cv[8] = "CV_";
         strncat(cv, name + 4, sizeof(cv) - 4);
         for (int i = 0; i < NUM_INPUTS; i++)
         {
            if (strcmp(cv, inputNames[i]) == 0)
               return i;
         }
         return -1;
      }

      void setLed(int index, bool on) { digitalWrite(led_pin[index], on ? HIGH : LOW); }

      bool buttonPressed(int index) { return digitalRead(switch_pin[index]) == LOW; }
   }
}

#endif
