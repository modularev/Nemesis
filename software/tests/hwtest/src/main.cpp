//
// Hardware test for the NEMESIS module
//
// - prints every control input (calibrated and raw), button and the encoder over USB serial (20 times per second)
// - the LED of each button is lit while the button is pressed
// - rev2 OLED: a bar per input (CV X..D, pots a..d), encoder position,
//   buttons and encoder switch; hold both buttons for 2 s to calibrate the knobs
// - audio channels 1 and 2 are passed through, every output can additionally
//   carry a DC level set over USB serial: "DC <output 0-7> <level -1..1>"
//

#include <nemesis.h>
#include "hwtest.h"

hwtest passthrough;

elapsedMillis sincePrint;

std::string dcPath[8]; // FAUST addresses of the dc0..dc7 sliders
char command[32];
size_t commandLength = 0;

void findDcParams()
{
   for (int p = 0; p < passthrough.getParamsCount(); p++)
   {
      std::string path = passthrough.getParamAddress(p);
      size_t pos = path.rfind("/dc");
      if (pos != std::string::npos && pos + 4 == path.size())
         dcPath[path[pos + 3] - '0'] = path;
   }
}

// "DC <output> <level>" sets a DC level on an output
void readCommands()
{
   while (Serial.available())
   {
      char c = Serial.read();
      if (c != '\n' && c != '\r')
      {
         if (commandLength < sizeof(command) - 1)
            command[commandLength++] = c;
         continue;
      }
      command[commandLength] = '\0';
      commandLength = 0;

      int output;
      float level;
      if (sscanf(command, "DC %d %f", &output, &level) == 2 && output >= 0 && output < 8 && !dcPath[output].empty())
      {
         passthrough.setParamValue(dcPath[output], constrain(level, -1.0f, 1.0f));
         Serial.printf("OK DC %d %.4f\n", output, level);
      }
   }
}

#if NEMESIS_HW_REV == 2
elapsedMillis sinceDraw;
constexpr uint32_t DRAW_INTERVAL_MS = 40;

constexpr int BAR_TOP = 0;
constexpr int BAR_HEIGHT = 23;
constexpr int BAR_WIDTH = 6;
constexpr int BAR_PITCH = 8;
constexpr int POT_GAP = 4;   // space between the CV and the pot bars
constexpr int PANEL_X = 96; // encoder and buttons

const char barLabels[] = "XYZABCDabcd";

int barX(int input)
{
   return input * BAR_PITCH + (input >= 7 ? POT_GAP : 0);
}

// small box with a label, filled while active
void drawIndicator(int x, int y, int w, const char *label, bool active)
{
   Adafruit_SSD1306 &d = nemesis::display;
   if (active)
      d.fillRect(x, y, w, 9, WHITE);
   else
      d.drawRect(x, y, w, 9, WHITE);
   d.setTextColor(active ? BLACK : WHITE);
   d.setCursor(x + (w - 6 * (int)strlen(label)) / 2 + 1, y + 1);
   d.print(label);
   d.setTextColor(WHITE);
}

void drawTestScreen()
{
   Adafruit_SSD1306 &d = nemesis::display;
   d.clearDisplay();
   d.setFont(); // built-in 5x7 font, cursor at the top left of a character

   for (int i = 0; i < nemesis::NUM_INPUTS; i++)
   {
      int x = barX(i);
      int h = (int)(nemesis::input(i) * (BAR_HEIGHT - 2) + 0.5f);
      d.drawRect(x, BAR_TOP, BAR_WIDTH, BAR_HEIGHT, WHITE);
      if (h > 0)
         d.fillRect(x + 1, BAR_TOP + BAR_HEIGHT - 1 - h, BAR_WIDTH - 2, h, WHITE);
      d.setCursor(x, BAR_TOP + BAR_HEIGHT + 2);
      d.print(barLabels[i]);
   }

   d.setCursor(PANEL_X, 0);
   d.printf("%+4ld", (long)nemesis::encoderPosition());
   drawIndicator(PANEL_X, 11, 15, "1", nemesis::buttonPressed(0));
   drawIndicator(PANEL_X + 17, 11, 15, "2", nemesis::buttonPressed(1));
   drawIndicator(PANEL_X, 23, 32, "ENC", nemesis::encoderPressed());

   d.display();
}
#endif

void setup()
{
   nemesis::begin(passthrough);
   findDcParams();
#if NEMESIS_HW_REV == 2
   nemesis::showMonitor(false);
#endif

   // blink all LEDs twice to check their polarity
   for (int n = 0; n < 2; n++)
   {
      for (int i = 0; i < nemesis::NUM_LEDS; i++)
         nemesis::setLed(i, true);
      delay(300);
      for (int i = 0; i < nemesis::NUM_LEDS; i++)
         nemesis::setLed(i, false);
      delay(300);
   }
}

void loop()
{
   nemesis::update();
   readCommands();

   for (int i = 0; i < nemesis::NUM_BUTTONS && i < nemesis::NUM_LEDS; i++)
      nemesis::setLed(i, nemesis::buttonPressed(i));

#if NEMESIS_HW_REV == 2
   if (sinceDraw >= DRAW_INTERVAL_MS)
   {
      sinceDraw = 0;
      drawTestScreen();
   }
#endif

   if (sincePrint >= 50)
   {
      sincePrint = 0;
      for (int i = 0; i < nemesis::NUM_INPUTS; i++)
         Serial.printf("%s %.4f  ", nemesis::inputName(i), nemesis::input(i));
      for (int i = 0; i < nemesis::NUM_BUTTONS; i++)
         Serial.printf("B%d %d  ", i + 1, nemesis::buttonPressed(i));
#if NEMESIS_HW_REV == 2
      Serial.printf("ENC %ld  ENCSW %d", (long)nemesis::encoderPosition(), nemesis::encoderPressed());
#endif
      Serial.println();
      // uncalibrated values with more digits, used by tests/cvtest
      Serial.print("RAW");
      for (int i = 0; i < nemesis::NUM_INPUTS; i++)
         Serial.printf(" %.5f", nemesis::rawInput(i));
      Serial.println();
   }
}
