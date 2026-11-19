// SPDX-License-Identifier: GPL-3.0-or-later
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
// released under GNU GPL v3 or later (see LICENSE)
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

         // LEDs dimmed by pulse density as on rev2 (pin 35 has no hardware
         // PWM): a 4 kHz timer adds each LED's duty (1/4096 units) to an
         // accumulator and lights the LED on overflow; updateLeds() shapes
         // on/off (5 ms up, 30 ms decay, gamma 2.2) and scales by the LED
         // brightness setting (1..10 = 3..100 % duty, off below 2 %)
         constexpr uint32_t LED_TICK_US = 250;
         constexpr uint8_t LED_ISR_PRIORITY = 64; // above the audio: the LEDs don't stall
         constexpr float LED_ATTACK_S = 0.005f;
         constexpr float LED_DECAY_S = 0.03f;
         IntervalTimer led_timer;
         volatile uint16_t led_duty[NUM_LEDS]; // 0..4096
         uint16_t led_acc[NUM_LEDS];
         bool led_on[NUM_LEDS];
         float led_env[NUM_LEDS];
         elapsedMicros since_leds;

         void ledTick()
         {
            led_acc[0] += led_duty[0];
            led_acc[1] += led_duty[1];
            led_acc[2] += led_duty[2];
            digitalWriteFast(35, led_acc[0] >= 4096 ? HIGH : LOW);
            digitalWriteFast(36, led_acc[1] >= 4096 ? HIGH : LOW);
            digitalWriteFast(37, led_acc[2] >= 4096 ? HIGH : LOW);
            led_acc[0] &= 4095;
            led_acc[1] &= 4095;
            led_acc[2] &= 4095;
         }

         void updateLeds()
         {
            float dt = since_leds * 1e-6f;
            since_leds = 0;
            float peak = 0.03f * powf(1.0f / 0.03f, (systemSettings().ledBrightness - 1) / 9.0f);
            for (int i = 0; i < NUM_LEDS; i++)
            {
               if (led_on[i])
                  led_env[i] = min(1.0f, led_env[i] + dt / LED_ATTACK_S);
               else
                  led_env[i] *= expf(-dt / LED_DECAY_S);
               float duty = peak * powf(led_env[i], 2.2f);
               led_duty[i] = duty < 0.02f ? 0 : (uint16_t)(duty * 4096.0f);
            }
         }

         uint16_t readings[NUM_INPUTS][WINDOW_SIZE];
         int reading_sum[NUM_INPUTS];
         int reading_index[NUM_INPUTS];
         float value[NUM_INPUTS];
         int current = 0;

         // !!!RANGE IS INVERTED!!! the reading at the knob minimum is the larger one
         uint16_t adc_min[NUM_INPUTS];
         uint16_t adc_max[NUM_INPUTS];
         bool calibrated[NUM_INPUTS] = {}; // per input: an input left out keeps the default scaling

         // remote calibration (KCAL): the extremes of the averaged readings while sweeping
         constexpr int MIN_SPAN = 100;  // readings a swept knob must cover
         constexpr int END_MARGIN = 2;  // inside the extremes: the ends reach 0 and 1
         bool sweeping = false;
         uint16_t sweep_lo[NUM_INPUTS];
         uint16_t sweep_hi[NUM_INPUTS];

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
               watchdogFeed(); // the calibration waits for the user
               updateLeds();
            }
            delay(50);
            while (digitalRead(switch_pin[index]) == LOW)
            {
               watchdogFeed();
               updateLeds();
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
            bool flag = EEPROM.read(EEPROM_FLAG) == 0xFF;
            for (int i = 0; i < NUM_INPUTS; i++)
            {
               adc_min[i] = readIntFromEEPROM(EEPROM_MIN + 2 * i);
               adc_max[i] = readIntFromEEPROM(EEPROM_MAX + 2 * i);
               // ignore implausible data, e.g. from an aborted calibration or an input left out
               calibrated[i] = flag && adc_min[i] >= adc_max[i] + MIN_SPAN && adc_min[i] <= ADC_MAX;
            }
         }

         float scale(int i, float raw)
         {
            float v = calibrated[i] ? (adc_min[i] - raw) / (float)(adc_min[i] - adc_max[i])
                                    : (ADC_MAX - raw) / (float)ADC_MAX;
            return constrain(v, 0.0f, 1.0f);
         }
      }

      FLASHMEM void begin(const Program &program)
      {
         setPSRAMspeed();
         delay(10);
         AudioMemory(NEMESIS_AUDIO_MEMORY);
         delay(10);

         for (int i = 0; i < NUM_LEDS; i++)
         {
            pinMode(led_pin[i], OUTPUT);
            digitalWriteFast(led_pin[i], LOW);
         }
         led_timer.priority(LED_ISR_PRIORITY);
         led_timer.begin(ledTick, LED_TICK_US);
         for (int i = 0; i < NUM_BUTTONS; i++)
            pinMode(switch_pin[i], INPUT);

         codec.enable();
         codecWrite(0x02, 0xFF);        // power down while configuring
         codecWrite(0x03, codecFunctionalMode()); // functional mode
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
         if (sweeping)
         {
            uint16_t a = (uint16_t)(averaged + 0.5f);
            sweep_lo[i] = min(sweep_lo[i], a);
            sweep_hi[i] = max(sweep_hi[i], a);
         }
         value[i] = SMOO_FACTOR * scale(i, averaged) + (1.0f - SMOO_FACTOR) * value[i];

         current = (current + 1) % NUM_INPUTS;
      }

      void service(const Program &program) { updateLeds(); }

      float input(int index) { return value[index]; }

      // rev1 calibrates while scaling the readings, there is no separate raw value
      float rawInput(int index) { return value[index]; }
      // not calibrated in volts: assume the 0..1 input spans 0..5 V
      float inputVolts(int index) { return value[index] * 5.0f; }

      void resetVoltCalibration() {}

      FLASHMEM void knobCalibration(const char *arg, Print &out)
      {
         auto averaged = [](int i)
         { return (uint16_t)(reading_sum[i] / (float)WINDOW_SIZE + 0.5f); };
         if (strcmp(arg, "START") == 0)
         {
            for (int i = 0; i < NUM_INPUTS; i++)
               sweep_lo[i] = sweep_hi[i] = averaged(i);
            sweeping = true;
         }
         else if (strcmp(arg, "CANCEL") == 0)
            sweeping = false;
         else if (strcmp(arg, "STORE") == 0 && sweeping)
         {
            sweeping = false;
            // inputs swept far enough take the new range, the others keep theirs
            for (int i = 0; i < NUM_INPUTS; i++)
            {
               if (sweep_hi[i] - sweep_lo[i] < MIN_SPAN)
                  continue;
               adc_min[i] = sweep_hi[i] - END_MARGIN; // inverted: the minimum reads high
               adc_max[i] = sweep_lo[i] + END_MARGIN;
            }
            for (int i = 0; i < NUM_INPUTS; i++)
            {
               bool keep = calibrated[i] || sweep_hi[i] - sweep_lo[i] >= MIN_SPAN;
               writeIntIntoEEPROM(EEPROM_MIN + 2 * i, keep ? adc_min[i] : 0);
               writeIntIntoEEPROM(EEPROM_MAX + 2 * i, keep ? adc_max[i] : 0);
            }
            EEPROM.write(EEPROM_FLAG, 0xFF);
            loadCalibration();
            watchdogFeed();
         }
         else if (strcmp(arg, "RESET") == 0)
         {
            sweeping = false;
            EEPROM.write(EEPROM_FLAG, 0);
            loadCalibration();
         }
         else if (*arg)
         {
            out.printf("? KCAL %s\n", arg);
            return;
         }
         // the state as one JSON line (raw readings: 10 bit, high at the knob minimum)
         auto list = [&out](const char *name, auto get)
         {
            out.printf(",\"%s\":[", name);
            for (int i = 0; i < NUM_INPUTS; i++)
            {
               if (i)
                  out.print(',');
               out.print(get(i));
            }
            out.print(']');
         };
         out.printf("KCAL {\"sweeping\":%s,\"minSpan\":%d", sweeping ? "true" : "false", MIN_SPAN);
         out.print(",\"names\":[");
         for (int i = 0; i < NUM_INPUTS; i++)
            out.printf("%s\"%s\"", i ? "," : "", inputNames[i]);
         out.print(']');
         list("raw", [&](int i) { return (int)averaged(i); });
         out.print(",\"value\":[");
         for (int i = 0; i < NUM_INPUTS; i++)
         {
            if (i)
               out.print(',');
            out.print(value[i], 4);
         }
         out.print(']');
         list("lo", [](int i) { return (int)sweep_lo[i]; });
         list("hi", [](int i) { return (int)sweep_hi[i]; });
         list("min", [](int i) { return (int)adc_min[i]; });
         list("max", [](int i) { return (int)adc_max[i]; });
         list("ok", [](int i) { return calibrated[i] ? 1 : 0; });
         out.println('}');
      }
   }

   // rev1 outputs are not calibrated, input polarity not measured
   void outputCorrection(int output, float &scale, float &offset)
   {
      scale = 1.0f;
      offset = 0.0f;
   }

   float inputScale(int input) { return 1.0f; }

   namespace hw
   {

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

      // button 1 held at power up
      bool forceFirstProgram()
      {
         pinMode(switch_pin[0], INPUT);
         return digitalRead(switch_pin[0]) == LOW;
      }

      void setLed(int index, bool on) { led_on[index] = on; } // dimmed and shaped: updateLeds()

      bool buttonPressed(int index) { return digitalRead(switch_pin[index]) == LOW; }
      // rev1 sums knob and CV in hardware and has no display to show a
      // pending catch: knobs act immediately
      bool isKnob(int index) { return false; }
   }

   // no display on rev1
   FLASHMEM void printScreen(Stream &out) { out.println("SCREEN none"); }
   FLASHMEM void printDisplayStats(Stream &out) {}
}

#endif
