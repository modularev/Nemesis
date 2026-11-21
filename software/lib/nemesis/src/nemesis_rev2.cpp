// SPDX-License-Identifier: GPL-3.0-or-later
//
// Library for the programmable Eurorack DSP Module NEMESIS
//
// nemesis_rev2.cpp
//
// Hardware revision 2: CV X Y Z A B C D on an LTC1867 SPI ADC, pots A-D on
// Teensy analog pins, a 128x32 SSD1306 OLED, two push buttons with LEDs and
// an encoder with push switch.
//
// Display: the current program and the CPU load. Turning the encoder opens
// the program settings (parameters without a knob, CV A-D routing), a click
// the program list, press and hold the system settings (transport run, clock,
// tempo, clock offset, MIDI channel, knob
// popup, brightness, CV A-D range). In a list a click edits the highlighted
// entry, turn to change it, click again when done. Settings are stored on
// the SD card. Turning a knob shows its parameter for a moment (a tick marks
// the knob while it has not caught the stored value). Knob values are saved
// per program and caught again after a switch. Programs with pages
// ([pager:1]): button 1 next page, the home screen shows the page and an
// activity strip, the list starts with the page's settings. Programs
// that use the transport show its clock and tempo on the home screen; run,
// clock source, tempo and offset are system settings.
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
// released under GNU GPL v3 or later (see LICENSE)
//

#include "nemesis.h"

#if NEMESIS_HW_REV == 2

#include "nemesis_hw.h"
#include <ADC.h>
#include <EEPROM.h>
#include <Encoder.h>
#include <SPI.h>
#include <Wire.h>
// menus, header, values: Oxanium 10 px (SIL OFL, fonts/OFL-Oxanium.txt), the
// small companion of Michroma (which turns 0/8 into D/B at small sizes)
#include "fonts/Oxanium10.h"
// program names: Michroma (SIL OFL, fonts/OFL-Michroma.txt), upright for the
// program, oblique for the knob popup; converted with fonts/ttf2gfx.py
#include "fonts/Michroma16.h"
#include "fonts/Michroma13Oblique.h"

namespace nemesis
{
   // I2C at 400 kHz while the display transfers (the library's default and
   // the SSD1306's specification): at 1 MHz frames took up to 358 ms
   // (2026-11-17, timeouts), so changes are sent only when the frame changed
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
         // the CV inputs are converted in a timer interrupt, independent of how
         // busy loop() is (a display transfer takes ~40 ms at 70 % audio load):
         // every channel each 7 * 125 us, smoothed with a ~4 ms time constant
         constexpr uint32_t LTC_SCAN_US = 125;
         constexpr uint8_t LTC_ISR_PRIORITY = 224; // below the audio update (208), waits while it runs
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
         constexpr uint8_t ADC_ISR_PRIORITY = 224; // below the audio update (IRQ_SOFTWARE 208; a lower number is a higher priority: 192 preempted the audio ~30000 times a second, mooSpace2 95 % instead of 83, 2026-11-17)

         // adaptive one pole filter: slow while a knob rests (removes the
         // remaining noise), fast while it is turned (no lag)
         constexpr float POT_TAU_REST = 0.05f; // seconds (0.02 let pot A flicker, 2026-11-17)
         constexpr float POT_SNAP_LSB = 64.0f; // difference for full speed, ~10x the rest noise (sd 6-7 LSB with the audio clock pickup; 24 let it through)
         // output in steps of 4 LSB (1024 steps, 10 bit), moving only when the
         // filtered value is 2 steps away: resting pots don't flicker (the
         // audio clocks couple into the wipers, hardware/rev2/KNOWN_ISSUES.md)
         constexpr float POT_STEP_LSB = 4.0f;
         constexpr float POT_HYSTERESIS_LSB = 2.0f * POT_STEP_LSB;

         constexpr int led_pin[NUM_LEDS] = {33, 34};

         // LEDs dimmed by pulse density (pin 34 has no hardware PWM): a 4 kHz
         // timer adds each LED's duty (1/4096 units) to an accumulator and
         // lights it on the carry; integer work on fixed pins, ~50 cycles a
         // tick (16 kHz with floats cost mooSpace2 ~4 points: it interrupts
         // the audio, above it so the LEDs don't stall). The loop shapes on/off
         // into an envelope: up in 5 ms, off decays (30 ms, gamma 2.2), the
         // peak from the LED brightness setting: 1..10 = 3 .. 100 % duty,
         // exponential (lower would flicker at 4 kHz); below 2 % it is off.
         constexpr uint32_t LED_TICK_US = 250;
         constexpr uint8_t LED_ISR_PRIORITY = 64;
         constexpr float LED_ATTACK_S = 0.005f;
         constexpr float LED_DECAY_S = 0.03f; // 80 ms stayed lit too long when blinking (the user)
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
            digitalWriteFast(33, led_acc[0] >= 4096 ? HIGH : LOW);
            digitalWriteFast(34, led_acc[1] >= 4096 ? HIGH : LOW);
            led_acc[0] &= 4095;
            led_acc[1] &= 4095;
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
         constexpr int switch_pin[NUM_BUTTONS] = {35, 36};
         constexpr int CODEC_ENABLE_PIN = 9;
         constexpr int ENC_A_PIN = 4;
         constexpr int ENC_B_PIN = 5;
         constexpr int ENC_SW_PIN = 6;

         constexpr uint32_t DISPLAY_INTERVAL_MS = 25; // redraws; sent only when changed (pushDisplay). 100 ms + transfer gave 8-9 frames/s: breaker's beat count (sixteenths, 125 ms at 120 BPM) skipped steps

         ADC adc;

         volatile float cv_val[CV_NUM]; // written in the timer interrupt
         int next_cv = 0;
         elapsedMicros since_conversion;
         IntervalTimer cv_timer;

         volatile float pot_val[POT_NUM]; // 0..1, written in the ADC interrupts

         // pots handled by one ADC module, converted round robin
         struct PotScanner
         {
            ADC_Module *module = nullptr;
            int pots[POT_NUM];
            int count = 0;
            int current = 0;
            float filtered[POT_NUM]; // in LSB
            float stepped[POT_NUM];  // the output, in POT_STEP_LSB steps
            float last[POT_NUM][4];  // the four conversions before, for the median
            bool primed[POT_NUM];
            float rest_coeff = 0.0f;
            elapsedMicros since_start;
         };
         PotScanner scanners[2];

         void potConverted(PotScanner &s)
         {
            int pot = s.pots[s.current];
            float now = s.module->readSingle();
            // median of the last 5 conversions: short bursts (one conversion
            // ~30 LSB off, 2026-11-11; two in a row 50-160 LSB off, from the
            // audio clock pickup, 2026-11-17) don't count as turning, can't
            // open the popup or move the stepped output; a real movement
            // arrives two conversions later (< 1 ms)
            float *h = s.last[s.current];
            if (!s.primed[s.current])
            {
               h[0] = h[1] = h[2] = h[3] = now;
               s.primed[s.current] = true;
            }
            float w[5] = {h[0], h[1], h[2], h[3], now};
            for (int i = 1; i < 5; i++) // insertion sort of 5
               for (int j = i; j > 0 && w[j] < w[j - 1]; j--)
               {
                  float t = w[j];
                  w[j] = w[j - 1];
                  w[j - 1] = t;
               }
            float x = w[2];
            h[0] = h[1];
            h[1] = h[2];
            h[2] = h[3];
            h[3] = now;
            float d = x - s.filtered[s.current];
            float snap = min(fabsf(d) / POT_SNAP_LSB, 1.0f);
            float a = s.rest_coeff + (1.0f - s.rest_coeff) * snap * snap;
            s.filtered[s.current] += a * d;
            float &out = s.stepped[s.current];
            if (fabsf(s.filtered[s.current] - out) > POT_HYSTERESIS_LSB)
               out = min(roundf(s.filtered[s.current] / POT_STEP_LSB) * POT_STEP_LSB, ADC_MAX);
            pot_val[pot] = out / ADC_MAX;

            s.current = (s.current + 1) % s.count;
            s.module->startSingleRead(pot_pin[s.pots[s.current]]);
         }

         void adc0Isr() { potConverted(scanners[0]); }
         void adc1Isr() { potConverted(scanners[1]); }

         bool monitor = true;
         elapsedMillis since_display;
         // STATUS: frames drawn since the last report, their time (drawing + transfer)
         uint32_t display_frames = 0, display_us = 0, display_max_us = 0;
         elapsedMillis display_stats_since;

         // screens: home (program, CPU load), program settings (turn the
         // encoder), program list (click), system settings (press and hold)
         enum Screen { HOME, SETTINGS, PROGRAMS, SYSTEM };
         constexpr uint32_t MENU_TIMEOUT_MS = 2000; // program settings and list close after this without input
         constexpr uint32_t PUSH_DEBOUNCE_MS = 30;
         constexpr uint32_t HOLD_MS = 600;
         constexpr uint32_t FAST_TURN_MS = 40; // detents closer than this: bigger steps
         Screen screen = HOME;
         bool editing = false;
         int cursor = 0;
         // where the lists were left: program settings per program, system
         constexpr int REMEMBERED = 32;
         int settings_cursor[REMEMBERED] = {};
         int system_cursor = 0;
         int32_t last_encoder = 0;
         bool last_push = false;
         bool hold_fired = false;
         elapsedMillis since_menu_input;
         elapsedMillis since_push_change;
         elapsedMillis since_turn;
         int brightness_shown = -1;

         // entries of the settings lists
         enum ItemKind { ITEM_SETTING, ITEM_ROUTE, ITEM_RUN, ITEM_CLOCK, ITEM_CLOCK_PULSES,
                         ITEM_TEMPO, ITEM_OFFSET, ITEM_MIDI, ITEM_POPUP, ITEM_KNOBS, ITEM_BRIGHTNESS, ITEM_LED_BRIGHTNESS, ITEM_CV_RANGE,
                         ITEM_BLOCK, ITEM_RATE, ITEM_SYSTEM, ITEM_SECTION };
         constexpr int BLOCK_CHOICES[] = {0, 32, 64, 128}; // 0: auto
         constexpr int PPQN_CHOICES[] = {1, 2, 4, 24};
         struct Item
         {
            ItemKind kind;
            int index; // control index (setting) or CV input (route)
         };
         constexpr int MAX_ITEMS = 64;
         constexpr int SCROLL_BAR_W = 4; // values end left of it
         Item items[MAX_ITEMS];
         int item_count = 0;
         int items_program = -1;
         int validCursor(int index); // the nearest entry that is not a section header

         // parameter overlay: a bound input that moves shows its parameter.
         // X/Y/Z sum knob and CV, so an input that keeps moving (patched
         // modulation) is ignored until it rests again.
         constexpr uint32_t PARAM_SHOW_MS = 1500;
         constexpr uint32_t ROLL_SHOW_MS = 1500;   // "roll #n" in the header after a roll
         constexpr uint32_t CLIP_SHOW_MS = 500;
         constexpr uint32_t PARAM_REDRAW_MS = 25;
         constexpr float PARAM_MOVE = 0.01f;       // change that counts as turning, ~40x the pot noise
         // after the start the inputs settle (X/Y/Z read through the CV ADC,
         // whose smoothing starts at 0): no popup for a turn before this
         constexpr uint32_t INPUT_SETTLE_MS = 1500;
         constexpr uint32_t PARAM_REST_MS = 500;   // still this long: the movement ended
         constexpr uint32_t PARAM_MOD_MS = 5000;   // moving longer than this: modulation, ignored
         struct InputWatch
         {
            float reference;
            elapsedMillis since_move;
            elapsedMillis moving; // since the movement started
            bool muted;
         };
         InputWatch watch[NUM_INPUTS];
         bool watch_valid = false;
         int param_input = -1; // input whose parameter is shown, -1 for none
         elapsedMillis since_param;

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
               s.stepped[s.count] = s.filtered[s.count];
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

         // sends the frame only when it changed (a checksum of the 512 bytes),
         // and at least once a second: redrawing every 25 ms costs ~1 ms of
         // drawing when nothing changes instead of a 12 ms I2C transfer
         uint32_t sent_sum = 0;
         elapsedMillis since_sent;
         void pushDisplay()
         {
            const uint8_t *b = display.getBuffer();
            uint32_t sum = 2166136261u; // FNV-1a
            for (int i = 0; i < 128 * 32 / 8; i++)
               sum = (sum ^ b[i]) * 16777619u;
            if (sum == sent_sum && since_sent < 1000)
               return;
            display.display();
            sent_sum = sum;
            since_sent = 0;
         }

         FLASHMEM void splash(const char *programName)
         {
            display.clearDisplay();
            display.setTextSize(1);
            display.setTextColor(WHITE);
            display.setFont(&Oxanium10);
            display.setCursor(0, 8);
            display.print(F("say hello to"));
            display.setCursor(0, 27);
            display.setFont(&Michroma13Oblique);
            display.print(F(">NEMESIS"));
            display.display();
            delay(1500);

            display.clearDisplay();
            display.setFont(&Oxanium10);
            display.setCursor(0, 8);
            display.print(F("running"));
            display.setCursor(0, 29);
            display.setFont(&Michroma16);
            display.print(programName);
            display.display();
            display.setFont(&Oxanium10);
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
            {
               watchdogFeed();
               scan();
            }
            delay(50);
         }

         // spans of the pots in percent
         FLASHMEM void drawCalibration(const float *lo, const float *hi)
         {
            display.clearDisplay();
            display.setFont(&Oxanium10); // a sketch may have changed it
            display.setTextColor(WHITE);
            display.setCursor(0, 8);
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
               watchdogFeed(); // the calibration waits for the user
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
            display.setFont(&Oxanium10);
            display.setCursor(0, 19);
            display.print(store ? F("calibration stored") : F("calibration cancelled"));
            display.display();
            setLed(0, false);
            waitForRelease();
            delay(700);
            watch_valid = false; // the knob scaling changed
         }

         // ---- screens ----------------------------------------------------

         Program &activeProgram() { return *program(currentProgram()); }

         // name of the menu entry with the largest value not above v, false if none
         bool menuName(const char *menu, float v, char *text, size_t size)
         {
            // menu: 'name':value;'name':value}
            bool found = false;
            float best = 0.0f;
            const char *p = menu;
            while (p && *p && *p != '}')
            {
               const char *open = strchr(p, '\'');
               const char *close = open ? strchr(open + 1, '\'') : nullptr;
               const char *colon = close ? strchr(close, ':') : nullptr;
               if (!colon)
                  break;
               float value = atof(colon + 1);
               if (value <= v + 1e-4f && (!found || value > best))
               {
                  found = true;
                  best = value;
                  snprintf(text, size, "%.*s", (int)min((size_t)(close - open - 1), size - 1), open + 1);
               }
               p = strchr(colon, ';');
               if (p)
                  p++;
            }
            return found;
         }

         // number of a parameter with up to 5 significant places, or its menu name
         void formatValue(const Control &c, float v, char *text, size_t size)
         {
            float a = fabsf(c.max) > fabsf(c.min) ? fabsf(c.max) : fabsf(c.min);
            if (c.action == Program::ACTION_ROLL)
            {
               snprintf(text, size, "#%d", (int)lroundf(v)); // position in the roll history
               return;
            }
            if (c.menu && menuName(c.menu, v, text, size))
               return;
            if (c.gate)
               snprintf(text, size, "%s", v > 0.5f ? "on" : "off");
            else if (c.digits >= 0)
               snprintf(text, size, "%.*f", c.digits, v);
            else if (c.log) // log ranges: 3 significant places
               snprintf(text, size, fabsf(v) < 10.0f ? "%.2f" : fabsf(v) < 100.0f ? "%.1f" : "%.0f", v);
            else
               snprintf(text, size, a < 10.0f ? "%.2f" : a < 1000.0f ? "%.1f" : "%.0f", v);
            if (c.unit)
               snprintf(text + strlen(text), size - strlen(text), " %s", c.unit);
         }

         // x for text ending at the right edge
         int16_t rightAligned(const char *text, int16_t y)
         {
            int16_t x1, y1;
            uint16_t w, h;
            display.getTextBounds(text, 0, y, &x1, &y1, &w, &h);
            return 128 - w - x1;
         }

         // a program with pages: the page number (Michroma), its name (the
         // value of the [pagename:] setting, e.g. "drumhead") and a strip of
         // the pages' activity at the right, the current one underlined
         FLASHMEM void drawPage(const Program &program)
         {
            const std::vector<Control> &controls = program.controls();
            int page = program.page();
            display.print(page);
            int x = display.getCursorX() + 6;
            const Control &pager = controls[program.pagerIndex()];
            int named = pager.pageName ? program.templateTarget(pager.pageName) : -1;
            display.setFont(&Oxanium10);
            if (named >= 0)
            {
               char text[24];
               formatValue(controls[named], controls[named].value, text, sizeof(text));
               display.setCursor(x, 29);
               display.print(text);
            }
            // strip: one bar per page with an [activity:N] bargraph
            int count = 0;
            for (const Indicator &i : program.indicators())
               if (i.activity >= 0)
                  count++;
            int k = 0;
            for (const Indicator &i : program.indicators())
            {
               if (i.activity < 0)
                  continue;
               int bx = 128 - (count - k) * 7;
               bool lit = *i.zone > 0.5f * (i.min + i.max);
               if (lit)
                  display.fillRect(bx, 17, 5, 10, WHITE);
               else
                  display.drawRect(bx, 17, 5, 10, WHITE);
               if (i.activity == page)
                  display.fillRect(bx, 29, 5, 2, WHITE);
               k++;
            }
         }

         // current program, its status values and the CPU load, a note when there is no SD card
         // the home setting's value (juno: "A11 Brass") as big as it fits:
         // Michroma 16, else 13 oblique, else Oxanium 10 (cut at the edge)
         FLASHMEM void drawHomeValue(const Program &program)
         {
            const Control &c = program.controls()[program.homeIndex()];
            char text[48];
            formatValue(c, c.value, text, sizeof(text));
            const GFXfont *fonts[] = {&Michroma16, &Michroma13Oblique, &Oxanium10};
            for (const GFXfont *f : fonts)
            {
               display.setFont(f);
               int16_t x1, y1;
               uint16_t w, h;
               display.getTextBounds(text, 0, 29, &x1, &y1, &w, &h);
               if (w <= 128 || f == &Oxanium10)
                  break;
            }
            display.setCursor(0, 29);
            display.print(text);
         }

         // ---- the home layout ('declare home "A|B";') --------------------

         // item k of the layout ('|' separated, spaces trimmed), false if none
         bool layoutItem(const char *layout, int k, char *item, size_t size)
         {
            for (int i = 0; layout && i < k; i++)
            {
               layout = strchr(layout, '|');
               if (layout)
                  layout++;
            }
            if (!layout || !*layout)
               return false;
            while (*layout == ' ')
               layout++;
            size_t n = 0;
            while (layout[n] && layout[n] != '|')
               n++;
            while (n > 0 && layout[n - 1] == ' ')
               n--;
            n = min(n, size - 1);
            memcpy(item, layout, n);
            item[n] = '\0';
            // "A=delay A": shown as A, the part after '=' names its controls
            char *eq = strchr(item, '=');
            if (eq)
            {
               *eq = '\0';
               for (char *e = eq - 1; e >= item && *e == ' '; e--)
                  *e = '\0';
            }
            return n > 0;
         }

         // the controls of layout item k ("A=delay A,depth": "delay A,depth";
         // without '=' the item itself), false if none
         bool layoutControls(const char *layout, int k, char *controls, size_t size)
         {
            for (int i = 0; layout && i < k; i++)
            {
               layout = strchr(layout, '|');
               if (layout)
                  layout++;
            }
            if (!layout || !*layout)
               return false;
            const char *end = strchr(layout, '|');
            size_t n = end ? (size_t)(end - layout) : strlen(layout);
            const char *eq = (const char *)memchr(layout, '=', n);
            if (eq)
            {
               n -= eq + 1 - layout;
               layout = eq + 1;
            }
            n = min(n, size - 1);
            memcpy(controls, layout, n);
            controls[n] = '\0';
            return true;
         }

         // a control whose value the home screen shows (its own label, or
         // named after '=' in the layout): turning it opens no popup
         bool controlOnHome(const Program &program, const Control &c)
         {
            char list[64];
            for (int k = 0; k < 3 && layoutControls(program.homeLayout(), k, list, sizeof(list)); k++)
            {
               for (char *name = strtok(list, ","); name; name = strtok(nullptr, ","))
               {
                  while (*name == ' ')
                     name++;
                  char *e = name + strlen(name);
                  while (e > name && e[-1] == ' ')
                     *--e = '\0';
                  if (strcmp(name, c.label) == 0 || strcmp(name, c.shown()) == 0)
                     return true;
               }
            }
            return false;
         }

         // a bargraph's label that the layout shows big (not in the top line)
         bool inLayout(const Program &program, const char *label)
         {
            char item[24];
            for (int k = 0; k < 3 && layoutItem(program.homeLayout(), k, item, sizeof(item)); k++)
               if (strcmp(item, label) == 0)
                  return true;
            return false;
         }

         // the value of a layout item without its unit, its caption, its unit
         void layoutValue(const Program &program, const char *item, char *value, size_t vsize, const char **unit)
         {
            *unit = nullptr;
            const TransportState &t = transportState();
            if (strcmp(item, "@tempo") == 0)
            {
               snprintf(value, vsize, "%.1f", t.bpm);
               *unit = "BPM";
               return;
            }
            if (strcmp(item, "@beat") == 0)
            {
               // bar.beat.sixteenth over the transport's 16 bars
               int sixteenths = (int)(t.position / 6.0);
               if (t.running)
                  snprintf(value, vsize, "%d.%d.%d", sixteenths / 16 % 16 + 1, sixteenths / 4 % 4 + 1, sixteenths % 4 + 1);
               else
                  snprintf(value, vsize, "stop");
               return;
            }
            for (const Indicator &i : program.indicators())
               if (strcmp(i.label, item) == 0)
               {
                  Control c = {};
                  c.min = i.min;
                  c.max = i.max;
                  c.menu = i.menu;
                  c.digits = i.digits;
                  formatValue(c, *i.zone, value, vsize);
                  *unit = i.unit;
                  return;
               }
            for (const Control &k : program.controls())
               if (strcmp(k.label, item) == 0 || strcmp(k.shown(), item) == 0)
               {
                  Control c = k;
                  c.unit = nullptr;
                  formatValue(c, k.input < 0 ? k.value : *k.zone, value, vsize);
                  *unit = k.unit;
                  return;
               }
            snprintf(value, vsize, "?");
         }

         int textWidth(const char *text)
         {
            int16_t x1, y1;
            uint16_t w, h;
            display.getTextBounds(text, 0, 20, &x1, &y1, &w, &h);
            return x1 + w;
         }

         // the big area under the top line: one value as big as it fits, two
         // or three side by side with a caption (name and unit) above each
         FLASHMEM void drawLayout(const Program &program)
         {
            char items[3][24];
            int n = 0;
            while (n < 3 && layoutItem(program.homeLayout(), n, items[n], sizeof(items[n])))
               n++;
            char value[32];
            const char *unit;
            if (n == 1)
            {
               layoutValue(program, items[0], value, sizeof(value), &unit);
               if (unit)
                  snprintf(value + strlen(value), sizeof(value) - strlen(value), " %s", unit);
               const GFXfont *fonts[] = {&Michroma16, &Michroma13Oblique, &Oxanium10};
               for (const GFXfont *f : fonts)
               {
                  display.setFont(f);
                  if (textWidth(value) <= 128)
                     break;
               }
               display.setCursor(0, 29);
               display.print(value);
               return;
            }
            int column = 128 / max(1, n);
            for (int k = 0; k < n; k++)
            {
               int x = k * column;
               layoutValue(program, items[k], value, sizeof(value), &unit);
               char caption[32];
               if (items[k][0] == '@')
                  snprintf(caption, sizeof(caption), "%s", unit ? unit : items[k] + 1);
               else if (unit)
                  snprintf(caption, sizeof(caption), "%s %s", items[k], unit);
               else
                  snprintf(caption, sizeof(caption), "%s", items[k]);
               display.setFont(&Oxanium10);
               display.setCursor(x, 17);
               display.print(caption);
               display.setFont(&Michroma13Oblique);
               if (textWidth(value) > column - 3)
                  display.setFont(&Oxanium10);
               display.setCursor(x, 29); // descenders (y) end 3 px lower
               display.print(value);
            }
         }

         FLASHMEM void drawHome()
         {
            char text[16];
            display.clearDisplay();
            display.setTextColor(WHITE);
            display.setFont(&Oxanium10);
            const Program &program = activeProgram();
            // 6pt reaches 8 px above the baseline; a first letter that reaches
            // left of its start (Oxanium's j) moves the line right
            int16_t x1, y1;
            uint16_t w, h;
            display.getTextBounds(program.name(), 0, 8, &x1, &y1, &w, &h);
            display.setCursor(x1 < 0 ? -x1 : 0, 8);
            bool paged = program.pagerIndex() >= 0;
            bool laid = !paged && program.homeLayout();
            bool homed = !paged && !laid && program.homeIndex() >= 0;
            if (paged || homed || laid) // the big line shows the page, the layout or the home setting: the program's name moves up
            {
               display.print(program.name());
               display.print(' ');
            }
            if (program.lastRollMillis() && millis() - program.lastRollMillis() < ROLL_SHOW_MS)
               display.printf("roll #%d ", program.rollPosition());
            if (sdProblem())
            {
               display.print(sdProblem());
               display.print(' ');
            }
            if (activeProgram().usesTransport())
            {
               const TransportState &t = transportState();
               display.print(t.cvClock ? F("IN8 ") : t.external ? F("MIDI ") : F("int "));
               if (laid && inLayout(program, "@tempo"))
                  ; // the tempo is in the big area
               else if (t.running)
                  display.printf("%.1f ", t.bpm);
               else
                  display.print(F("stop "));
            }
            float limit_db = 0; // strongest gain reduction of the program's limiters
            for (const Indicator &i : activeProgram().indicators())
            {
               if (i.limit) // linear gain of the limiter
                  limit_db = min(limit_db, 20.0f * log10f(max(*i.zone, 0.001f)));
               if (i.led >= 0 || i.limit || i.activity >= 0 || (laid && inLayout(program, i.label)))
                  continue;
               Control c = {};
               c.min = i.min;
               c.max = i.max;
               c.menu = i.menu;
               c.unit = i.unit;
               c.digits = i.digits;
               formatValue(c, *i.zone, text, sizeof(text));
               display.print(text);
               display.print(' ');
            }
            // top right: CLIP after an output clipped, else limiting, else the
            // CPU load as a number in the built-in 5 x 7 font (small: "cpu 9%"
            // took a third of the line, a bar said too little)
            bool clip = millis() - activeProgram().lastClipMillis() < CLIP_SHOW_MS && activeProgram().lastClipMillis() != 0;
            if (!clip && limit_db >= -0.5f)
            {
               // the largest block of the last 500 ms: how close the program
               // comes to a late block (the average hides programs whose
               // work comes in some blocks, like an FFT frame every 4).
               // Updated twice a second: changing every frame, it made every
               // frame differ and be sent (pushDisplay)
               static int shown_load = 0;
               static elapsedMillis since_load = 1000;
               if (since_load >= 500)
               {
                  shown_load = (int)constrain(cpuLoadPeak() + 0.5f, 0.0f, 999.0f);
                  since_load = 0;
               }
               snprintf(text, sizeof(text), "%d%%", shown_load);
               display.setFont(nullptr); // classic font: the cursor is the top left corner
               display.setCursor(128 - 6 * (int)strlen(text), 1); // 5 px glyphs, 1 px apart: the last one ends at 127
               display.print(text);
               display.setFont(&Michroma16);
               text[0] = '\0';
            }
            else if (clip)
               snprintf(text, sizeof(text), "CLIP");
            else
               snprintf(text, sizeof(text), "lim %d dB", (int)roundf(limit_db));
            int16_t x = text[0] ? rightAligned(text, 8) : 128;
            if (clip)
            {
               display.fillRect(x - 2, 0, 130 - x, 10, WHITE);
               display.setTextColor(BLACK);
            }
            display.setCursor(x, 8);
            display.print(text);
            display.setTextColor(WHITE);
            display.setFont(&Michroma16);
            display.setCursor(0, 29); // descenders (g, j, p) end 3 px below the baseline
            if (laid)
               drawLayout(program);
            else if (homed)
               drawHomeValue(program);
            else if (!paged)
               display.print(program.name());
            else
               drawPage(program);
            display.setFont(&Oxanium10);
            pushDisplay();
         }

         // the control bound to a hardware input
         const Control *controlOf(const Program &program, int input)
         {
            for (const Control &c : program.controls())
               if (c.input == input)
                  return &c;
            return nullptr;
         }

         // finds a bound input that is being turned
         void watchInputs(const Program &program)
         {
            for (int i = 0; i < NUM_INPUTS; i++)
            {
               InputWatch &w = watch[i];
               float v = input(i);
               if (!watch_valid || millis() < INPUT_SETTLE_MS)
               {
                  w.reference = v;
                  continue;
               }
               if (fabsf(v - w.reference) < PARAM_MOVE)
               {
                  if (w.since_move > PARAM_REST_MS)
                     w.muted = false;
                  continue;
               }
               w.reference = v;
               if (w.since_move > PARAM_REST_MS)
                  w.moving = 0;
               w.since_move = 0;
               if (w.moving > PARAM_MOD_MS)
                  w.muted = true;
               if (w.muted || !controlOf(program, i))
                  continue;
               if (param_input != i)
                  since_display = DISPLAY_INTERVAL_MS; // draw now
               param_input = i;
               since_param = 0;
            }
            watch_valid = millis() >= INPUT_SETTLE_MS;
         }

         // parameter name (large if it fits), a bar and the value; a page's
         // setting is named with its page ("3 decay"); knob >= 0: where a
         // knob that has not caught the value yet stands (a tick)
         FLASHMEM void drawParameter(const Control &c, float knob = -1.0f)
         {
            int16_t x1, y1;
            uint16_t w, h;
            char name[40];
            int titled = c.title ? activeProgram().controlIndex(c.title) : -1;
            if (titled >= 0)
            {
               // an assignable knob: named after what it controls
               const Control &t = activeProgram().controls()[titled];
               formatValue(t, t.value, name, sizeof(name));
            }
            else if (c.condEquals)
               snprintf(name, sizeof(name), "%d %s", (int)lroundf(c.condValue), c.shown());
            else
               snprintf(name, sizeof(name), "%s", c.shown());
            display.clearDisplay();
            display.setTextColor(WHITE);
            display.setFont(&Michroma13Oblique); // names up to ~11 characters, else the small font
            display.getTextBounds(name, 0, 20, &x1, &y1, &w, &h);
            if (x1 + w > 128)
            {
               display.setFont(&Oxanium10);
               display.setCursor(0, 14);
            }
            else
               display.setCursor(0, 20);
            display.print(name);

            // settings show their value (the zone follows only later)
            float shown = c.input < 0 ? c.value : *c.zone;
            float position = constrain(c.position(shown), 0.0f, 1.0f);
            display.drawRect(0, 26, 92, 6, WHITE);
            display.fillRect(1, 27, (int)(position * 90.0f + 0.5f), 4, WHITE);
            if (knob >= 0.0f)
            {
               // the knob's place: turn it there to take over
               int x = 1 + (int)(constrain(knob, 0.0f, 1.0f) * 89.0f + 0.5f);
               display.drawFastVLine(x, 23, 3, WHITE);
               display.drawFastVLine(x, 27, 4, INVERSE);
            }

            char text[16];
            formatValue(c, shown, text, sizeof(text));
            display.setFont(&Oxanium10);
            display.setCursor(rightAligned(text, 31), 31);
            display.print(text);
            pushDisplay();
         }

         // ---- settings lists ---------------------------------------------

         FLASHMEM void buildItems()
         {
            item_count = 0;
            items_program = currentProgram();
            if (screen == SETTINGS)
            {
               // a program with pages lists the current page's settings first,
               // then the others; [section:] starts a header row (a setting
               // without one stays in the section before it)
               const Program &p = activeProgram();
               const std::vector<Control> &controls = p.controls();
               bool paged = p.pagerIndex() >= 0;
               // a preset choice ([action:preset]) comes first
               for (size_t i = 0; i < controls.size() && item_count < MAX_ITEMS - NUM_ROUTABLE_CV - 2; i++)
                  if (controls[i].action == Program::ACTION_PRESET && controls[i].input < 0 && p.visible(controls[i]))
                     items[item_count++] = {ITEM_SETTING, (int)i};
               for (int pass = paged ? 0 : 1; pass < 2; pass++)
               {
                  const char *section = nullptr;
                  bool any = false;
                  for (size_t i = 0; i < controls.size() && item_count < MAX_ITEMS - NUM_ROUTABLE_CV - 2; i++)
                  {
                     const Control &c = controls[i];
                     if (c.input >= 0 || !c.listed || !p.visible(c) || c.action == Program::ACTION_PRESET)
                        continue;
                     bool onPage = paged && c.condEquals && p.isCondition(controls[p.pagerIndex()], c);
                     if (onPage != (pass == 0) && paged)
                        continue;
                     const char *name = c.section ? c.section : section;
                     if (!any && !name && pass == 1 && paged)
                        name = "global";
                     if (name && (!section || strcmp(name, section) != 0))
                        items[item_count++] = {ITEM_SECTION, c.section ? (int)i : -1};
                     section = name;
                     any = true;
                     items[item_count++] = {ITEM_SETTING, (int)i};
                  }
               }
               for (int i = 0; i < NUM_ROUTABLE_CV; i++)
                  items[item_count++] = {ITEM_ROUTE, routableCV(i)};
               items[item_count++] = {ITEM_SYSTEM, 0}; // last row: the system settings
            }
            else if (screen == SYSTEM)
            {
               items[item_count++] = {ITEM_RUN, 0};
               items[item_count++] = {ITEM_CLOCK, 0};
               if (systemSettings().clockSource == TransportCore::CV)
                  items[item_count++] = {ITEM_CLOCK_PULSES, 0};
               items[item_count++] = {ITEM_TEMPO, 0};
               items[item_count++] = {ITEM_OFFSET, 0};
               items[item_count++] = {ITEM_MIDI, 0};
               items[item_count++] = {ITEM_POPUP, 0};
               items[item_count++] = {ITEM_KNOBS, 0};
               items[item_count++] = {ITEM_BRIGHTNESS, 0};
               items[item_count++] = {ITEM_LED_BRIGHTNESS, 0};
               items[item_count++] = {ITEM_CV_RANGE, 0};
               items[item_count++] = {ITEM_BLOCK, 0};
               items[item_count++] = {ITEM_RATE, 0};
            }
         }

         const char *itemLabel(const Item &item, char *text, size_t size)
         {
            switch (item.kind)
            {
            case ITEM_SETTING:
               return activeProgram().controls()[item.index].shown();
            case ITEM_SECTION:
               return item.index >= 0 ? activeProgram().controls()[item.index].section : "global";
            case ITEM_ROUTE:
               snprintf(text, size, "CV %c", 'A' + item.index - routableCV(0));
               return text;
            case ITEM_RUN:
               return "run";
            case ITEM_CLOCK:
               return "clock";
            case ITEM_CLOCK_PULSES:
               return "IN 8 pulses";
            case ITEM_TEMPO:
               return "tempo";
            case ITEM_OFFSET:
               return "clock offset";
            case ITEM_MIDI:
               return "MIDI channel";
            case ITEM_POPUP:
               return "knob popup";
            case ITEM_KNOBS:
               return "knobs";
            case ITEM_BRIGHTNESS:
               return "brightness";
            case ITEM_LED_BRIGHTNESS:
               return "LED brightness";
            case ITEM_CV_RANGE:
               return "CV A-D range";
            case ITEM_BLOCK:
               return "block size";
            case ITEM_RATE:
               return "sample rate";
            case ITEM_SYSTEM:
               return "system";
            }
            return "";
         }

         void itemValue(const Item &item, char *text, size_t size)
         {
            const SystemSettings &s = systemSettings();
            switch (item.kind)
            {
            case ITEM_SECTION:
            {
               // a page's section: the page ("3"), else nothing
               const Program &p = activeProgram();
               const Control *c = item.index >= 0 ? &p.controls()[item.index] : nullptr;
               if (c && p.pagerIndex() >= 0 && c->condEquals)
                  snprintf(text, size, "%d", p.page());
               else
                  text[0] = '\0';
               break;
            }
            case ITEM_SETTING:
            {
               const Control &c = activeProgram().controls()[item.index];
               formatValue(c, c.value, text, size);
               break;
            }
            case ITEM_ROUTE:
            {
               int target = activeProgram().route(item.index);
               snprintf(text, size, "%s", target >= 0 ? activeProgram().controls()[target].shown() : "off");
               break;
            }
            case ITEM_RUN:
               snprintf(text, size, "%s", s.run ? "on" : "off");
               break;
            case ITEM_CLOCK:
            {
               static const char *const names[] = {"auto", "internal", "MIDI", "IN 8"};
               snprintf(text, size, "%s", names[constrain(s.clockSource, 0, 3)]);
               break;
            }
            case ITEM_CLOCK_PULSES:
               if (s.clockPpqn == 24)
                  snprintf(text, size, "24 ppqn");
               else
                  snprintf(text, size, "1/%d", 4 * s.clockPpqn);
               break;
            case ITEM_TEMPO:
            {
               // following a clock: its tempo, marked with *
               const TransportState &t = transportState();
               if (t.external)
                  snprintf(text, size, "%.1f*", t.bpm);
               else
                  snprintf(text, size, "%.0f", s.tempo);
               break;
            }
            case ITEM_OFFSET:
               snprintf(text, size, "%+d ms", s.clockOffsetMs);
               break;
            case ITEM_MIDI:
               if (s.midiChannel == 0)
                  snprintf(text, size, "omni");
               else
                  snprintf(text, size, "%d", s.midiChannel);
               break;
            case ITEM_POPUP:
               snprintf(text, size, "%s", s.popup ? "on" : "off");
               break;
            case ITEM_KNOBS:
               snprintf(text, size, "%s", s.knobJump ? "jump" : "catch");
               break;
            case ITEM_BRIGHTNESS:
               snprintf(text, size, "%d", s.brightness);
               break;
            case ITEM_LED_BRIGHTNESS:
               snprintf(text, size, "%d", s.ledBrightness);
               break;
            case ITEM_CV_RANGE:
               snprintf(text, size, "0-%dV", s.cvRangeVolts);
               break;
            case ITEM_BLOCK:
               // auto: the running program's (or 64)
               if (s.blockSize == 0)
                  snprintf(text, size, "auto %d", wantedBlockSize());
               else
                  snprintf(text, size, "%d", s.blockSize);
               break;
            case ITEM_RATE:
               // auto: the running program's (or 48 kHz)
               if (s.sampleRate == 0)
                  snprintf(text, size, "auto %dk", wantedSampleRate(nullptr) / 1000);
               else
                  snprintf(text, size, "%dk", s.sampleRate / 1000);
               break;
            case ITEM_SYSTEM:
               snprintf(text, size, ">");
               break;
            }
         }

         // settings with [if:]/[ifnot:] on this one come and go
         void rebuildIfConditional(const Program &p, const Control &c)
         {
            for (const Control &other : p.controls())
               if (p.isCondition(c, other))
               {
                  buildItems();
                  cursor = validCursor(cursor);
                  return;
               }
         }

         // turning while editing; fast turns move the settings in bigger steps
         void changeItem(const Item &item, int delta, bool fast)
         {
            SystemSettings &s = systemSettings();
            Program &p = activeProgram();
            switch (item.kind)
            {
            case ITEM_SETTING:
            {
               Control &c = p.controls()[item.index];
               if (c.action == Program::ACTION_ROLL)
               {
                  // every detent right a new roll, left one back
                  for (int k = 0; k < abs(delta); k++)
                     if (delta > 0)
                        p.roll(p.rollTarget());
                     else
                        p.rollBack();
                  programSettingsChanged(p);
                  return;
               }
               if (c.gate)
                  c.value = delta > 0 ? 1.0f : 0.0f;
               else if (c.log) // even steps along the knob travel
                  c.value = c.fromPosition(constrain(c.position(c.value) + delta * (fast ? 0.05f : 0.01f), 0.0f, 1.0f));
               else
               {
                  float lo = min(c.min, c.max), hi = max(c.min, c.max);
                  // the FAUST step when it is coarse (integers like a tempo), else 1 %
                  float step = (c.step >= (hi - lo) / 250.0f ? c.step : (hi - lo) / 100.0f) * (fast ? 5 : 1);
                  float v = c.value + delta * step;
                  if (c.step > 0.0f)
                     v = c.min + roundf((v - c.min) / c.step) * c.step;
                  c.value = constrain(v, lo, hi);
               }
               c.changed = true;
               programSettingsChanged(p);
               rebuildIfConditional(p, c);
               return;
            }
            case ITEM_ROUTE:
            {
               // off, then every parameter except the one this CV input drives itself
               int n = p.controls().size();
               int target = p.route(item.index);
               for (int k = 0; k < abs(delta); k++)
               {
                  int t = target;
                  do
                     t += delta > 0 ? 1 : -1;
                  while (t >= 0 && t < n && p.controls()[t].input == item.index);
                  if (t < -1 || t >= n)
                     break;
                  target = t;
               }
               p.setRoute(item.index, target);
               programSettingsChanged(p);
               return;
            }
            case ITEM_RUN:
               s.run = delta > 0;
               break;
            case ITEM_CLOCK:
               s.clockSource = constrain(s.clockSource + delta, 0, 3);
               buildItems(); // the CV clock items come and go
               break;
            case ITEM_CLOCK_PULSES:
            {
               int k = 0;
               while (k < 3 && PPQN_CHOICES[k] != s.clockPpqn)
                  k++;
               s.clockPpqn = PPQN_CHOICES[constrain(k + delta, 0, 3)];
               break;
            }
            case ITEM_TEMPO:
               s.tempo = constrain(roundf(s.tempo) + delta * (fast ? 5 : 1), 20.0f, 300.0f);
               break;
            case ITEM_OFFSET:
               s.clockOffsetMs = constrain(s.clockOffsetMs + delta, -50, 50);
               break;
            case ITEM_MIDI:
               s.midiChannel = constrain(s.midiChannel + delta, 0, 16);
               break;
            case ITEM_POPUP:
               s.popup = delta > 0;
               break;
            case ITEM_KNOBS:
               s.knobJump = delta > 0;
               break;
            case ITEM_BRIGHTNESS:
               s.brightness = constrain(s.brightness + delta, 1, 10);
               break;
            case ITEM_LED_BRIGHTNESS:
               s.ledBrightness = constrain(s.ledBrightness + delta, 1, 10);
               break;
            case ITEM_CV_RANGE:
               s.cvRangeVolts = delta > 0 ? 8 : 5;
               break;
            case ITEM_SECTION:
               return;
            case ITEM_BLOCK:
            {
               int k = 0;
               while (k < 3 && BLOCK_CHOICES[k] != s.blockSize)
                  k++;
               s.blockSize = BLOCK_CHOICES[constrain(k + delta, 0, 3)]; // applied by the library
               break;
            }
            case ITEM_RATE:
            {
               static const int RATES[] = {0, 48000, 96000}; // 0: auto
               int k = 0;
               while (k < 2 && RATES[k] != s.sampleRate)
                  k++;
               s.sampleRate = RATES[constrain(k + delta, 0, 2)]; // the library restarts the program
               break;
            }
            }
            systemSettingsChanged();
         }

         // scroll bar at the right edge when a list has more than three rows
         void drawScrollBar(int first, int count)
         {
            if (count <= 3)
               return;
            int h = max(4, 32 * 3 / count);
            int y = (32 - h) * first / (count - 3);
            display.drawFastVLine(127, 0, 32, WHITE);
            display.fillRect(125, y, 3, h, WHITE);
         }

         // three rows around the cursor: label left, value right (inverted
         // while editing), the scroll bar at the edge
         FLASHMEM void drawList()
         {
            char label[24], value[24];
            display.clearDisplay();
            display.setTextColor(WHITE);
            display.setFont(&Oxanium10);
            if (item_count == 0)
            {
               display.setCursor(0, 19);
               display.print(F("no settings"));
            }
            int first = constrain(cursor - 1, 0, max(0, item_count - 3));
            for (int row = 0; row < 3 && first + row < item_count; row++)
            {
               int index = first + row;
               int y = 8 + row * 11; // Oxanium: 8 px above, 2 below: rows 0..31
               if (items[index].kind == ITEM_SECTION)
               {
                  // header: name, a rule, the page number at the right
                  itemValue(items[index], value, sizeof(value));
                  display.setCursor(2, y);
                  display.print(itemLabel(items[index], label, sizeof(label)));
                  int x0 = display.getCursorX() + 3;
                  int x1 = (value[0] ? rightAligned(value, y) - SCROLL_BAR_W : 128 - SCROLL_BAR_W) - 3;
                  if (x1 > x0)
                     display.drawFastHLine(x0, y - 3, x1 - x0, WHITE);
                  if (value[0])
                  {
                     display.setCursor(rightAligned(value, y) - SCROLL_BAR_W, y);
                     display.print(value);
                  }
                  continue;
               }
               bool selected = index == cursor;
               display.setCursor(0, y);
               display.print(selected ? '>' : ' ');
               display.print(itemLabel(items[index], label, sizeof(label)));
               itemValue(items[index], value, sizeof(value));
               int x = rightAligned(value, y) - SCROLL_BAR_W;
               bool inverted = selected && editing;
               display.fillRect(x - 3, y - 9, 128 - SCROLL_BAR_W - (x - 3), 11, inverted ? WHITE : BLACK);
               display.setTextColor(inverted ? BLACK : WHITE);
               display.setCursor(x, y);
               display.print(value);
               display.setTextColor(WHITE);
            }
            drawScrollBar(first, item_count);
            pushDisplay();
         }

         // three entries around the cursor, '>' marks the cursor, '*' the running program
         FLASHMEM void drawPrograms()
         {
            display.clearDisplay();
            display.setTextColor(WHITE);
            display.setFont(&Oxanium10);
            int n = programCount();
            int first = constrain(cursor - 1, 0, max(0, n - 3));
            for (int row = 0; row < 3; row++)
            {
               int index = first + row;
               if (index < 0 || index >= n)
                  continue;
               display.setCursor(0, 8 + row * 11);
               display.printf("%c%c %s", index == cursor ? '>' : ' ',
                              index == currentProgram() ? '*' : ' ', program(index)->name());
            }
            drawScrollBar(first, n);
            pushDisplay();
         }

         // ---- encoder ----------------------------------------------------

         // the cursor moves over settings, not section headers
         int skipSections(int index, int direction)
         {
            while (index >= 0 && index < item_count && items[index].kind == ITEM_SECTION)
               index += direction;
            return index;
         }

         int moveCursor(int from, int delta)
         {
            int direction = delta > 0 ? 1 : -1;
            int at = from;
            for (int k = 0; k < abs(delta); k++)
            {
               int next = skipSections(at + direction, direction);
               if (next < 0 || next >= item_count)
                  break;
               at = next;
            }
            return at;
         }

         int validCursor(int index)
         {
            index = constrain(index, 0, max(0, item_count - 1));
            int down = skipSections(index, 1);
            if (down < item_count)
               return down;
            int up = skipSections(index, -1);
            return up >= 0 ? up : 0;
         }

         void open(Screen next)
         {
            screen = next;
            editing = false;
            buildItems();
            if (next == PROGRAMS)
               cursor = currentProgram();
            else if (next == SETTINGS)
               cursor = currentProgram() < REMEMBERED ? settings_cursor[currentProgram()] : 0;
            else
               cursor = system_cursor;
            cursor = constrain(cursor, 0, max(0, (next == PROGRAMS ? programCount() : item_count) - 1));
            if (next != PROGRAMS)
               cursor = validCursor(cursor);
         }

         // number of entries of a 'name':value;.. menu
         int menuEntries(const char *menu)
         {
            int n = 0;
            for (const char *p = menu; p && *p && *p != '}'; p++)
               if (*p == ':')
                  n++;
            return n;
         }

         // the home setting one step per detent (its menu entries, wrapping)
         void changeHome(int delta)
         {
            Program &p = activeProgram();
            Control &c = p.controls()[p.homeIndex()];
            float lo = min(c.min, c.max), hi = max(c.min, c.max);
            float step = c.step > 0.0f ? c.step : 1.0f;
            int n = (int)lroundf((hi - lo) / step) + 1;
            if (c.menu && menuEntries(c.menu) > 0)
               n = min(n, menuEntries(c.menu)); // presets: as many as are loaded
            int k = (int)lroundf((c.value - lo) / step);
            k = ((k + delta) % n + n) % n;
            c.value = lo + k * step;
            c.changed = true;
            programSettingsChanged(p);
            since_display = DISPLAY_INTERVAL_MS; // draw now
         }

         void onTurn(int delta, bool fast)
         {
            switch (screen)
            {
            case HOME:
               // a program's [home:1] setting, else the settings list
               if (activeProgram().homeIndex() >= 0)
                  changeHome(delta);
               else
                  open(SETTINGS);
               break;
            case PROGRAMS:
            {
               int n = programCount();
               if (n > 0)
                  cursor = ((cursor + delta) % n + n) % n;
               break;
            }
            case SETTINGS:
            case SYSTEM:
               if (editing)
                  changeItem(items[cursor], delta, fast);
               else
                  cursor = moveCursor(cursor, delta);
               if (screen == SYSTEM)
                  system_cursor = cursor;
               else if (currentProgram() >= 0 && currentProgram() < REMEMBERED)
                  settings_cursor[currentProgram()] = cursor;
               break;
            }
         }

         void onClick()
         {
            switch (screen)
            {
            case HOME:
               open(SETTINGS);
               break;
            case PROGRAMS:
               if (cursor != currentProgram())
               {
                  display.clearDisplay();
                  display.setFont(&Oxanium10);
                  display.setCursor(0, 19);
                  display.printf("loading %s", program(cursor)->name());
                  display.display();
                  selectProgram(cursor);
               }
               screen = HOME;
               break;
            case SETTINGS:
            case SYSTEM:
               if (item_count == 0 || items[cursor].kind == ITEM_SECTION)
                  break;
               if (items[cursor].kind == ITEM_SYSTEM)
               {
                  open(SYSTEM);
                  break;
               }
               editing = !editing;
               if (!editing && items[cursor].kind == ITEM_SETTING)
               {
                  // confirmed: an action setting acts (randomize) and resets
                  Program &p = activeProgram();
                  Control &c = p.controls()[items[cursor].index];
                  if (c.action == Program::ACTION_RANDOMIZE)
                  {
                     p.actionDone(c);
                     programSettingsChanged(p);
                  }
               }
               if (editing && items[cursor].kind == ITEM_SETTING)
               {
                  // start from the parameter, a MIDI CC may have changed it
                  Control &c = activeProgram().controls()[items[cursor].index];
                  if (c.cvMask == 0)
                     c.value = *c.zone;
               }
               break;
            }
         }

         // hold: the program list from the home screen and the settings, the
         // system settings from the program list, back home from them
         void onHold()
         {
            if (screen == SYSTEM)
               screen = HOME;
            else if (screen == PROGRAMS)
               open(SYSTEM);
            else
               open(PROGRAMS);
         }



         // home: turn = the program's [home:1] setting (else the settings),
         // click = settings, hold = program list (there: hold = system
         // settings, also the settings' last row); in a list click = edit
         void handleEncoder()
         {
            int32_t position = encoderPosition();
            int delta = position - last_encoder;
            last_encoder = position;
            bool any = false;
            if (delta)
            {
               bool fast = since_turn < FAST_TURN_MS;
               since_turn = 0;
               onTurn(delta, fast);
               any = true;
            }

            bool push = encoderPressed();
            if (push != last_push && since_push_change > PUSH_DEBOUNCE_MS)
            {
               last_push = push;
               since_push_change = 0;
               if (push)
                  hold_fired = false;
               else if (!hold_fired)
               {
                  onClick();
                  any = true;
               }
            }
            if (push && last_push && !hold_fired && since_push_change >= HOLD_MS)
            {
               hold_fired = true;
               onHold();
               any = true;
            }

            if (any)
            {
               since_menu_input = 0;
               since_display = DISPLAY_INTERVAL_MS; // redraw now
            }
            // back home after a while (not from the system settings or while
            // editing a value), or when the program was changed elsewhere
            bool stale = screen == SETTINGS && items_program != currentProgram();
            bool idle = (screen == SETTINGS || screen == PROGRAMS) && !editing && since_menu_input > MENU_TIMEOUT_MS;
            if (screen != HOME && (idle || stale))
            {
               screen = HOME;
               editing = false;
               since_display = DISPLAY_INTERVAL_MS;
            }
         }

         // display contrast for brightness 1..10, roughly even steps to the eye
         void applyBrightness()
         {
            int level = systemSettings().brightness;
            if (level == brightness_shown)
               return;
            brightness_shown = level;
            display.ssd1306_command(SSD1306_SETCONTRAST);
            display.ssd1306_command(max(1, level * level * 255 / 100));
         }

         // The display froze once while the sound and the firmware went on
         // (2026-11-17, turning POT A in harmonizer): the SSD1306 probably took
         // a disturbed I2C byte as a command (another addressing mode, a
         // scroll) and ignored the frames from then on. Every 2 s its setup
         // is sent again: Adafruit_SSD1306::begin()'s sequence for 128 x 32
         // without "display off" (no flash), then the contrast.
         constexpr uint32_t DISPLAY_SETUP_MS = 2000;
         elapsedMillis since_setup;
         void refreshDisplaySetup()
         {
            if (since_setup < DISPLAY_SETUP_MS)
               return;
            since_setup = 0;
            static const uint8_t setup[] = {
                SSD1306_SETDISPLAYCLOCKDIV, 0x80, SSD1306_SETMULTIPLEX, 31,
                SSD1306_SETDISPLAYOFFSET, 0x00, SSD1306_SETSTARTLINE | 0x0,
                SSD1306_CHARGEPUMP, 0x14, SSD1306_MEMORYMODE, 0x00,
                SSD1306_SEGREMAP | 0x1, SSD1306_COMSCANDEC, SSD1306_SETCOMPINS, 0x02,
                SSD1306_SETPRECHARGE, 0xF1, SSD1306_SETVCOMDETECT, 0x40,
                SSD1306_DISPLAYALLON_RESUME, SSD1306_NORMALDISPLAY,
                SSD1306_DEACTIVATE_SCROLL, SSD1306_DISPLAYON};
            for (uint8_t c : setup)
               display.ssd1306_command(c);
            brightness_shown = -1; // applyBrightness() sends the contrast again
         }
      }

      FLASHMEM void begin(const Program &program)
      {
         setPSRAMspeed();

         for (int i = 0; i < NUM_LEDS; i++)
         {
            pinMode(led_pin[i], OUTPUT);
            digitalWriteFast(led_pin[i], LOW);
            setLed(i, false);
         }
         led_timer.priority(LED_ISR_PRIORITY);
         led_timer.begin(ledTick, LED_TICK_US);
         for (int i = 0; i < NUM_BUTTONS; i++)
            pinMode(switch_pin[i], INPUT);
         pinMode(ENC_SW_PIN, INPUT_PULLUP);
         pinMode(CODEC_ENABLE_PIN, OUTPUT);

         display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
         display.setTextWrap(false);
         splash(program.name());

         pinMode(PIN_SPI_SS, OUTPUT);
         digitalWriteFast(PIN_SPI_SS, HIGH);
         SPI.begin();
         transferLTC(next_cv); // dummy transfer to start the first conversion

         beginPots();
         cv_timer.priority(LTC_ISR_PRIORITY);
         cv_timer.begin(scanCV, LTC_SCAN_US);

         loadCalibration();
         if (buttonPressed(0))
            calibrate();

         digitalWrite(CODEC_ENABLE_PIN, HIGH);
         AudioMemory(NEMESIS_AUDIO_MEMORY);
         codec.enable();
         codecWrite(0x03, codecFunctionalMode()); // MCLK range of the sample rate the program asked for
         codecWrite(0x05, 0x00);        // ADC control
         codecWrite(0x06, 0x43);        // volume and mute changes as a soft ramp (~21 ms at 48k); the
                                        // library's 0x63 waited for zero crossings: quiet or offset
                                        // channels unmuted slowly, one after the other
         codec.invertDAC(0xFF);         // the output stages invert, measured on rev2
         codec.volume(1.0);
      }

      // pots are read in the ADC interrupts, CV inputs in a timer interrupt
      void scan()
      {
      }

      void service(const Program &program)
      {
         if (!(buttonPressed(0) && buttonPressed(1)))
            both_buttons_held = 0;
         else if (both_buttons_held >= CALIBRATION_HOLD_MS)
            calibrate();

         updateLeds();
         if (!monitor)
            return;
         refreshDisplaySetup();
         applyBrightness();
         handleEncoder();
         watchInputs(program);
         if (param_input >= 0 && since_param >= PARAM_SHOW_MS)
         {
            param_input = -1;
            since_display = DISPLAY_INTERVAL_MS;
         }
         // button 1 changed the page: the list follows (same place), redraw
         static int shown_page = -1;
         if (program.page() != shown_page)
         {
            shown_page = program.page();
            if (screen == SETTINGS)
            {
               buildItems();
               cursor = validCursor(cursor);
            }
            since_display = DISPLAY_INTERVAL_MS;
         }
         bool popup = screen == HOME && systemSettings().popup && param_input >= 0;
         const Control *param = popup ? controlOf(program, param_input) : nullptr;
         float knob = -1.0f; // a knob that has not caught its value yet
         if (param && param->knob && !param->caught)
            knob = constrain(input(param_input), 0.0f, 1.0f);
         if (param && param->edit)
         {
            // an [edit:] knob shows the setting it changes
            int target = program.editTarget(*param);
            param = target >= 0 ? &program.controls()[target] : nullptr;
         }
         // the home screen shows its value already (declare home): no popup
         if (param && program.homeLayout() && program.pagerIndex() < 0 && controlOnHome(program, *param))
            param = nullptr;
         bool strip = screen == HOME && program.pagerIndex() >= 0; // flashes of the activity strip
         if (since_display >= (param || strip ? PARAM_REDRAW_MS : DISPLAY_INTERVAL_MS))
         {
            uint32_t draw_start = micros();
            if (screen == PROGRAMS)
               drawPrograms();
            else if (screen == SETTINGS || screen == SYSTEM)
               drawList();
            else if (param)
               drawParameter(*param, knob);
            else
               drawHome();
            uint32_t took = micros() - draw_start;
            display_frames++;
            display_us += took;
            display_max_us = max(display_max_us, took);
            since_display = 0; // counted from the end of the transfer, so loop() gets time in between
         }
      }

      // CV A-D can't read below 0 V: on some of them 0 V itself lies just
      // beyond the end of the ADC range (calibrated zero outside 0..1), so an
      // unpatched jack would read a few 10 mV. A reading at that end is 0 V.
      constexpr float ADC_EDGE = 2.0f / 16383.0f;
      float cvVolts(int index)
      {
         float r = raw(index), zero = volts.cv_zero[index];
         if (index >= CV_BIPOLAR && ((zero < ADC_EDGE && r < ADC_EDGE) || (zero > 1.0f - ADC_EDGE && r > 1.0f - ADC_EDGE)))
            return 0.0f;
         return (r - zero) / volts.cv_per_volt[index];
      }

      float input(int index)
      {
         float v;
         if (index < CV_NUM)
         {
            if (index < CV_BIPOLAR)
               v = (cvVolts(index) + CV_SPAN_VOLTS / 2) / CV_SPAN_VOLTS;
            else // CV A-D: 0..5 V or 0..8 V, a system setting
               v = cvVolts(index) / systemSettings().cvRangeVolts;
         }
         else
            v = (raw(index) - calibration.lo[index]) / (calibration.hi[index] - calibration.lo[index]);
         return constrain(v, 0.0f, 1.0f);
      }

      FLASHMEM void resetVoltCalibration() { resetVolts(); }

      float rawInput(int index) { return raw(index); }
      // CV inputs calibrated, the knobs as 0..5 V
      float inputVolts(int index) { return index < CV_NUM ? cvVolts(index) : input(index) * 5.0f; }

      int aliasIndex(const char *name) { return -1; }

      // encoder held at power up (the pin is configured in begin(), so set it up here too)
      bool forceFirstProgram()
      {
         pinMode(ENC_SW_PIN, INPUT_PULLUP);
         delayMicroseconds(50);
         return digitalRead(ENC_SW_PIN) == LOW;
      }

      // LEDs in the push buttons are active high (checked on hardware)
      void setLed(int index, bool on) { led_on[index] = on; } // dimmed and shaped: updateLeds()

      bool buttonPressed(int index) { return digitalRead(switch_pin[index]) == LOW; }
      // X/Y/Z (knob + CV) and POT A-D; CV A-D are jacks only
      bool isKnob(int index) { return (index >= 0 && index <= 2) || (index >= 7 && index <= 10); }
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

   // the display's framebuffer as one line "SCREEN <hex>" (128 x 32, SSD1306
   // pages: byte x + 128 * (y / 8), bit y % 8), to look at the screen remotely
   FLASHMEM void printDisplayStats(Stream &out)
   {
      float secs = hw::display_stats_since / 1000.0f;
      out.printf("display: %.1f frames/s, %.1f ms per frame (max %.1f ms, drawing + I2C transfer)\n",
                 hw::display_frames / max(secs, 0.001f), hw::display_frames ? hw::display_us / 1000.0f / hw::display_frames : 0.0f,
                 hw::display_max_us / 1000.0f);
      hw::display_frames = hw::display_us = hw::display_max_us = 0;
      hw::display_stats_since = 0;
   }

   FLASHMEM void printScreen(Stream &out)
   {
      const uint8_t *b = display.getBuffer();
      out.print("SCREEN ");
      for (int i = 0; i < 128 * 32 / 8; i++)
         out.printf("%02x", b[i]);
      out.println();
   }

   FLASHMEM void printVoltCalibration(Stream &out)
   {
      for (int i = 0; i < hw::CV_NUM; i++)
         out.printf("%s zero %.5f per volt %.5f\n", hw::inputNames[i], hw::volts.cv_zero[i], hw::volts.cv_per_volt[i]);
      for (int i = 0; i < NUM_OUTPUTS; i++)
         out.printf("OUT%d %.4f V per unit, %+.4f V at 0\n", i + 1, hw::volts.out_per_unit[i], hw::volts.out_offset[i]);
   }

   // all 8 audio input stages invert (IN 1-6 on the main board, IN 7/8 through
   // the input board and CS5343, measured on rev2); the CS5343 channels can't
   // be inverted in the codec, so it's done in software for all of them
   float inputScale(int input) { return -1.0f; }

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
