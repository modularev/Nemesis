//
// Pin scanner: reads every Teensy pin that the NEMESIS rev2 doesn't use yet
// (with pull-up) and prints how often each one changed in the last 250 ms.
// Turn or press the unknown control and watch which pins toggle.
//

#include <Arduino.h>

// skipped: 7 8 20 21 23 TDM, 9 codec enable, 10-13 SPI, 18 19 I2C,
// 33 34 LEDs, 35 36 buttons, 38-41 pots, 48-54 PSRAM
const int pins[] = {0, 1, 2, 3, 4, 5, 6, 14, 15, 16, 17, 22, 24, 25, 26, 27, 28, 29, 30, 31, 32, 37};
constexpr int NUM = sizeof(pins) / sizeof(pins[0]);

int last[NUM];
int changes[NUM];
elapsedMillis sinceReport;

void setup()
{
   for (int i = 0; i < NUM; i++)
   {
      pinMode(pins[i], INPUT_PULLUP);
   }
   delay(10);
   for (int i = 0; i < NUM; i++)
      last[i] = digitalReadFast(pins[i]);
}

void loop()
{
   for (int i = 0; i < NUM; i++)
   {
      int v = digitalReadFast(pins[i]);
      if (v != last[i])
      {
         changes[i]++;
         last[i] = v;
      }
   }

   if (sinceReport >= 250)
   {
      sinceReport = 0;
      Serial.print("state ");
      for (int i = 0; i < NUM; i++)
         Serial.printf("%d=%d ", pins[i], last[i]);
      Serial.print(" changed ");
      for (int i = 0; i < NUM; i++)
      {
         if (changes[i])
            Serial.printf("%d:%d ", pins[i], changes[i]);
         changes[i] = 0;
      }
      Serial.println();
   }
}
