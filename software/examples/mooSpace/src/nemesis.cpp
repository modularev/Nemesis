// 
// Library for the programmable Eurorack DSP Module NEMESIS
//
// src/nemesis.cpp
//
// Copyright © 2021 Arev Imer if not mentioned elsewise
// released under GNU GPL v3
//

#include "nemesis.h"
#include <utility/imxrt_hw.h>

uint16_t nemesis::adc_min[9];
uint16_t nemesis::adc_max[9];

float clamp(float n)
{
   if (n < 0.0)
      return 0.0;
   else if (n > 1.0)
      return 1.0;
   else
      return n;
}

void writeIntIntoEEPROM(uint8_t address, uint16_t number)
{
   EEPROM.write(address, number >> 8);
   EEPROM.write(address + 1, number & 0xFF);
}

uint16_t readIntFromEEPROM(uint8_t address)
{
   return (EEPROM.read(address) << 8) + EEPROM.read(address + 1);
}

bool nemesis::init(void)
{
   // set pin modes
   for (int i = 0; i < 3; i++)
   {
      pinMode(led_pin[i], OUTPUT);
      pinMode(switch_pin[i], INPUT);
   }


   // initialize 
   for (int i = 0; i < 9; i++)
   {
      adc_min[i] = 1021;
      adc_max[i] = 5;
   }   

   setPSRAMspeed();
   return true;
}

uint16_t nemesis::getADC_min(int idx){
   return adc_min[idx];
}

uint16_t nemesis::getADC_max(int idx){
   return adc_max[idx];
}


void nemesis::calibration(void)
{
   if (digitalRead(switch_pin[2]) == LOW)
   {
      digitalWrite(led_pin[1], HIGH); // light led to show calibration started

      Serial.println("Turn all potentiometers to minimum position, then press t1");
      while (digitalRead(switch_pin[0]) == HIGH)
      {
      }

      Serial.println("deleting old calibration data");
      for (int i = 0; i < EEPROM.length(); i++)
      {
         EEPROM.write(i, 0);
      }

      // store minimal value !!!RANGE IS INVERTED!!!
      for (int i = 0; i < 9; i++)
      {
         adc_min[i] = analogRead(analog_pin[i]);
         adc_min[i] += analogRead(analog_pin[i]);
         adc_min[i] += analogRead(analog_pin[i]);
         adc_min[i] += analogRead(analog_pin[i]);
         adc_min[i] = adc_min[i] >> 2; 
         writeIntIntoEEPROM(2 * i + 10, adc_min[i]);
      }

      Serial.println("Turn all potentiometers to maximum position, then press t2");
      digitalWrite(led_pin[0], HIGH); // another led to indicate 2nd step in calibration
      while (digitalRead(switch_pin[1]) == HIGH)
      {
      }

      // store maximal value !!!RANGE IS INVERTED!!!
      for (int i = 0; i < 9; i++)
      {
         adc_max[i] = analogRead(analog_pin[i]);
         adc_max[i] += analogRead(analog_pin[i]);
         adc_max[i] += analogRead(analog_pin[i]);
         adc_max[i] += analogRead(analog_pin[i]);
         adc_max[i] = adc_max[i] >> 2; 
         writeIntIntoEEPROM(2 * i + 30, adc_max[i]);
      }

      EEPROM.write(0, 0xFF); // set EEPROM to 0xFF to check if calibration has been done

      for (int i = 0 ; i < 9; i++){
         Serial.print(adc_min[i]);
         Serial.print("\t");
      }
      Serial.println();

      for (int i = 0 ; i < 9; i++){
         Serial.print(adc_max[i]);
         Serial.print("\t");
      }

      Serial.println("to exit calibration press button");
      while (digitalRead(switch_pin[2]) == HIGH)
      {
      }  
      digitalWrite(led_pin[0], LOW);
      digitalWrite(led_pin[1], LOW);
   }
}

void nemesis::loadCalibration(void)
{
   for (int i = 0; i < 9; i++)
   {
      adc_min[i] = readIntFromEEPROM(2 * i + 10);
      adc_max[i] = readIntFromEEPROM(2 * i + 30);
   }
}

void nemesis::setPSRAMspeed(void)
{
   //Set SPI to 180Mhz
   CCM_CCGR7 |= CCM_CCGR7_FLEXSPI2(CCM_CCGR_OFF);
   CCM_CBCMR = (CCM_CBCMR & ~(CCM_CBCMR_FLEXSPI2_PODF_MASK | CCM_CBCMR_FLEXSPI2_CLK_SEL_MASK)) | CCM_CBCMR_FLEXSPI2_PODF(3) // 4 and 2 133MHz
               | CCM_CBCMR_FLEXSPI2_CLK_SEL(1);
   CCM_CCGR7 |= CCM_CCGR7_FLEXSPI2(CCM_CCGR_ON);
}

void nemesis::setSampleRate(int freq)
{ // code by Frank B https://forum.pjrc.com/threads/38753-Discussion-about-a-simple-way-to-change-the-sample-rate?p=266346&viewfull=1#post266346
   // PLL between 27*24 = 648MHz und 54*24=1296MHz
   int n1 = 4; //SAI prescaler 4 => (n1*n2) = multiple of 4
   int n2 = 1 + (24000000 * 27) / (freq * 256 * n1);
   double C = ((double)freq * 256 * n1 * n2) / 24000000;
   int c0 = C;
   int c2 = 10000;
   int c1 = C * c2 - (c0 * c2);
   set_audioClock(c0, c1, c2, true);
   CCM_CS1CDR = (CCM_CS1CDR & ~(CCM_CS1CDR_SAI1_CLK_PRED_MASK | CCM_CS1CDR_SAI1_CLK_PODF_MASK)) | CCM_CS1CDR_SAI1_CLK_PRED(n1 - 1) // &0x07
                | CCM_CS1CDR_SAI1_CLK_PODF(n2 - 1);                                                                                // &0x3f
}