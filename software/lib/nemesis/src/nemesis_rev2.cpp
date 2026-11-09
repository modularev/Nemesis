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
// the SD card. Turning a knob shows its parameter for a moment. Programs
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
                         ITEM_TEMPO, ITEM_OFFSET, ITEM_MIDI, ITEM_POPUP, ITEM_BRIGHTNESS, ITEM_CV_RANGE,
                         ITEM_BLOCK };
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

         // parameter overlay: a bound input that moves shows its parameter.
         // X/Y/Z sum knob and CV, so an input that keeps moving (patched
         // modulation) is ignored until it rests again.
         constexpr uint32_t PARAM_SHOW_MS = 1500;
         constexpr uint32_t CLIP_SHOW_MS = 500;
         constexpr uint32_t PARAM_REDRAW_MS = 40;
         constexpr float PARAM_MOVE = 0.01f;       // change that counts as turning, ~40x the pot noise
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
            display.setCursor(0, 8);
            display.print(F("say hello to"));
            display.setCursor(0, 29);
            display.setFont(&SourceCodePro_ExtraLightItalic14pt7b);
            display.print(F(">NEMESIS"));
            display.display();
            delay(1500);

            display.clearDisplay();
            display.setFont(&SourceCodePro_ExtraLight6pt7b);
            display.setCursor(0, 8);
            display.print(F("running"));
            display.setCursor(0, 29);
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
            if (c.menu && menuName(c.menu, v, text, size))
               return;
            if (c.gate)
               snprintf(text, size, "%s", v > 0.5f ? "on" : "off");
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

         // current program, its status values and the CPU load, a note when there is no SD card
         FLASHMEM void drawHome()
         {
            char text[16];
            display.clearDisplay();
            display.setTextColor(WHITE);
            display.setFont(&SourceCodePro_ExtraLight6pt7b);
            display.setCursor(0, 8); // 6pt reaches 8 px above the baseline
            if (!sdCardPresent())
               display.print(F("no SD "));
            if (activeProgram().usesTransport())
            {
               const TransportState &t = transportState();
               display.print(t.cvClock ? F("IN8 ") : t.external ? F("MIDI ") : F("int "));
               if (t.running)
                  display.printf("%.1f ", t.bpm);
               else
                  display.print(F("stop "));
            }
            float limit_db = 0; // strongest gain reduction of the program's limiters
            for (const Indicator &i : activeProgram().indicators())
            {
               if (i.limit) // linear gain of the limiter
                  limit_db = min(limit_db, 20.0f * log10f(max(*i.zone, 0.001f)));
               if (i.led >= 0 || i.limit)
                  continue;
               Control c = {};
               c.min = i.min;
               c.max = i.max;
               c.menu = i.menu;
               c.unit = i.unit;
               formatValue(c, *i.zone, text, sizeof(text));
               display.print(text);
               display.print(' ');
            }
            // top right: CLIP after an output clipped, else limiting, else the CPU load
            bool clip = millis() - activeProgram().lastClipMillis() < CLIP_SHOW_MS && activeProgram().lastClipMillis() != 0;
            if (clip)
               snprintf(text, sizeof(text), "CLIP");
            else if (limit_db < -0.5f)
               snprintf(text, sizeof(text), "lim %d dB", (int)roundf(limit_db));
            else
               snprintf(text, sizeof(text), "cpu %d%%", (int)cpuLoad());
            int16_t x = rightAligned(text, 8);
            if (clip)
            {
               display.fillRect(x - 2, 0, 130 - x, 10, WHITE);
               display.setTextColor(BLACK);
            }
            display.setCursor(x, 8);
            display.print(text);
            display.setTextColor(WHITE);
            display.setFont(&SourceCodePro_ExtraLightItalic14pt7b);
            display.setCursor(0, 29); // descenders (g, p) end 2 px below the baseline
            display.print(activeProgram().name());
            display.setFont(&SourceCodePro_ExtraLight6pt7b);
            display.display();
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
               if (!watch_valid)
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
            watch_valid = true;
         }

         // parameter name (large if it fits), a bar and the value
         FLASHMEM void drawParameter(const Control &c)
         {
            int16_t x1, y1;
            uint16_t w, h;
            display.clearDisplay();
            display.setTextColor(WHITE);
            display.setFont(&SourceCodePro_ExtraLightItalic14pt7b);
            // 14pt ascenders (b, d, f, l) reach 20 px above the baseline
            display.getTextBounds(c.label, 0, 20, &x1, &y1, &w, &h);
            if (x1 + w > 128)
            {
               display.setFont(&SourceCodePro_ExtraLight6pt7b);
               display.setCursor(0, 14);
            }
            else
               display.setCursor(0, 20);
            display.print(c.label);

            float position = constrain(c.position(*c.zone), 0.0f, 1.0f);
            display.drawRect(0, 26, 92, 6, WHITE);
            display.fillRect(1, 27, (int)(position * 90.0f + 0.5f), 4, WHITE);

            char text[16];
            formatValue(c, *c.zone, text, sizeof(text));
            display.setFont(&SourceCodePro_ExtraLight6pt7b);
            display.setCursor(rightAligned(text, 31), 31);
            display.print(text);
            display.display();
         }

         // ---- settings lists ---------------------------------------------

         void buildItems()
         {
            item_count = 0;
            items_program = currentProgram();
            if (screen == SETTINGS)
            {
               const std::vector<Control> &controls = activeProgram().controls();
               for (size_t i = 0; i < controls.size() && item_count < MAX_ITEMS - NUM_ROUTABLE_CV; i++)
                  if (controls[i].input < 0 && controls[i].listed)
                     items[item_count++] = {ITEM_SETTING, (int)i};
               for (int i = 0; i < NUM_ROUTABLE_CV; i++)
                  items[item_count++] = {ITEM_ROUTE, routableCV(i)};
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
               items[item_count++] = {ITEM_BRIGHTNESS, 0};
               items[item_count++] = {ITEM_CV_RANGE, 0};
               items[item_count++] = {ITEM_BLOCK, 0};
            }
         }

         const char *itemLabel(const Item &item, char *text, size_t size)
         {
            switch (item.kind)
            {
            case ITEM_SETTING:
               return activeProgram().controls()[item.index].label;
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
            case ITEM_BRIGHTNESS:
               return "brightness";
            case ITEM_CV_RANGE:
               return "CV A-D range";
            case ITEM_BLOCK:
               return "block size";
            }
            return "";
         }

         void itemValue(const Item &item, char *text, size_t size)
         {
            const SystemSettings &s = systemSettings();
            switch (item.kind)
            {
            case ITEM_SETTING:
            {
               const Control &c = activeProgram().controls()[item.index];
               formatValue(c, c.value, text, size);
               break;
            }
            case ITEM_ROUTE:
            {
               int target = activeProgram().route(item.index);
               snprintf(text, size, "%s", target >= 0 ? activeProgram().controls()[target].label : "off");
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
            case ITEM_BRIGHTNESS:
               snprintf(text, size, "%d", s.brightness);
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
            case ITEM_BRIGHTNESS:
               s.brightness = constrain(s.brightness + delta, 1, 10);
               break;
            case ITEM_CV_RANGE:
               s.cvRangeVolts = delta > 0 ? 8 : 5;
               break;
            case ITEM_BLOCK:
            {
               int k = 0;
               while (k < 3 && BLOCK_CHOICES[k] != s.blockSize)
                  k++;
               s.blockSize = BLOCK_CHOICES[constrain(k + delta, 0, 3)]; // applied by the library
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
            display.setFont(&SourceCodePro_ExtraLight6pt7b);
            if (item_count == 0)
            {
               display.setCursor(0, 19);
               display.print(F("no settings"));
            }
            int first = constrain(cursor - 1, 0, max(0, item_count - 3));
            for (int row = 0; row < 3 && first + row < item_count; row++)
            {
               int index = first + row;
               int y = 9 + row * 11;
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
            display.display();
         }

         // three entries around the cursor, '>' marks the cursor, '*' the running program
         FLASHMEM void drawPrograms()
         {
            display.clearDisplay();
            display.setTextColor(WHITE);
            display.setFont(&SourceCodePro_ExtraLight6pt7b);
            int n = programCount();
            int first = constrain(cursor - 1, 0, max(0, n - 3));
            for (int row = 0; row < 3; row++)
            {
               int index = first + row;
               if (index < 0 || index >= n)
                  continue;
               display.setCursor(0, 9 + row * 11);
               display.printf("%c%c %s", index == cursor ? '>' : ' ',
                              index == currentProgram() ? '*' : ' ', program(index)->name());
            }
            drawScrollBar(first, n);
            display.display();
         }

         // ---- encoder ----------------------------------------------------

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
         }

         void onTurn(int delta, bool fast)
         {
            switch (screen)
            {
            case HOME:
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
                  cursor = constrain(cursor + delta, 0, max(0, item_count - 1));
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
               open(PROGRAMS);
               break;
            case PROGRAMS:
               if (cursor != currentProgram())
               {
                  display.clearDisplay();
                  display.setFont(&SourceCodePro_ExtraLight6pt7b);
                  display.setCursor(0, 19);
                  display.printf("loading %s", program(cursor)->name());
                  display.display();
                  selectProgram(cursor);
               }
               screen = HOME;
               break;
            case SETTINGS:
            case SYSTEM:
               if (item_count == 0)
                  break;
               editing = !editing;
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

         void onHold()
         {
            if (screen == SYSTEM)
               screen = HOME;
            else
               open(SYSTEM);
         }

         // turn: settings, click: program list (or edit in a list), hold: system settings
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
         codecWrite(0x03, 0xF4 | 0x01); // functional mode
         codecWrite(0x05, 0x00);        // ADC control
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

         if (!monitor)
            return;
         applyBrightness();
         handleEncoder();
         watchInputs(program);
         if (param_input >= 0 && since_param >= PARAM_SHOW_MS)
         {
            param_input = -1;
            since_display = DISPLAY_INTERVAL_MS;
         }
         bool popup = screen == HOME && systemSettings().popup && param_input >= 0;
         const Control *param = popup ? controlOf(program, param_input) : nullptr;
         if (since_display >= (param ? PARAM_REDRAW_MS : DISPLAY_INTERVAL_MS))
         {
            if (screen == PROGRAMS)
               drawPrograms();
            else if (screen == SETTINGS || screen == SYSTEM)
               drawList();
            else if (param)
               drawParameter(*param);
            else
               drawHome();
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
