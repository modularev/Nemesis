//
// Library for the programmable Eurorack DSP Module NEMESIS v0.2
//
// src/nemesis_hw.cpp
//
// Copyright © 2021 Arev Imer if not mentioned elsewise
// released under GNU GPL v3
//

#include "nemesis_hw.h"
#include <utility/imxrt_hw.h>

AudioControlCS42448 nemesis::codec;
Adafruit_SSD1306 display(128, 32, &Wire, -1);

float nemesis::cv_val[7];// = {.0f, .0f, .0f, .0f, .0f, .0f, .0f};
float nemesis::pot_val[4];// = {.0f, .0f, .0f, .0f};



bool nemesis::init(void)
{
   pinMode(9, OUTPUT);
   digitalWrite(9, HIGH);

   Serial.print("hi there");
   pinMode(led_pin[0], OUTPUT);
   digitalWrite(led_pin[0], HIGH);

   display.begin(SSD1306_SWITCHCAPVCC, 0x3C);

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
   //Serial.println("we came this far");

   AudioMemory(100); // 832 max
   codec.enable();
   initADC();

   for (int i = 0; i < 7; i++)
   {
      cv_val[i] = 0;
   }
   for (int i = 0; i < 4; i++)
   {
      pot_val[i] = 0;
   }




   // set pin modes
   // for (int i = 0; i < 3; i++)
   // {
   //    pinMode(led_pin[i], OUTPUT);
   //    pinMode(switch_pin[i], INPUT);
   // }

   // initialize
   // for (int i = 0; i < 9; i++)
   // {
   //    adc_min[i] = 1021;
   //    adc_max[i] = 5;
   // }

   setPSRAMspeed();
   return true;
}

void nemesis::initADC(void)
{
   pinMode(PIN_SPI_SS, OUTPUT);
   SPI.begin();
   SPI.transfer16((uint16_t)((LTC1867_CH0 | LTC1867_UNIPOLAR_MODE) << 8));
}

uint16_t nemesis::getADC_raw(uint8_t adc_command)
{
   return SPI.transfer16((uint16_t)((adc_command | LTC1867_UNIPOLAR_MODE) << 8));
}

float nemesis::scale_ADC(uint16_t raw_val)
{
   return (~raw_val) * INT16_DIV;
}

void nemesis::scan_CV(void)
{
   cv_val[0] = scale_ADC(getADC_raw(LTC1867_CH1)); // invert reading to acommodate for inverting opamp
   cv_val[1] = scale_ADC(getADC_raw(LTC1867_CH2));
   cv_val[2] = scale_ADC(getADC_raw(LTC1867_CH3));
   cv_val[3] = scale_ADC(getADC_raw(LTC1867_CH4));
   cv_val[4] = scale_ADC(getADC_raw(LTC1867_CH5));
   cv_val[5] = scale_ADC(getADC_raw(LTC1867_CH6));
   cv_val[6] = scale_ADC(getADC_raw(LTC1867_CH0));
}

void nemesis::scan_POT(void)
{
   for (int i = 0; i < 3; i++)
   {
      uint16_t raw_pot = analogRead(pot_pin[i]);
      pot_val[i] = (~(raw_pot >> 1) & 0b111111111) * INT9_DIV;
   }
}

void nemesis::setPSRAMspeed(void)
{
   // Set SPI to 180Mhz
   CCM_CCGR7 |= CCM_CCGR7_FLEXSPI2(CCM_CCGR_OFF);
   CCM_CBCMR = (CCM_CBCMR & ~(CCM_CBCMR_FLEXSPI2_PODF_MASK | CCM_CBCMR_FLEXSPI2_CLK_SEL_MASK)) | CCM_CBCMR_FLEXSPI2_PODF(3) // 4 and 2 133MHz
               | CCM_CBCMR_FLEXSPI2_CLK_SEL(1);
   CCM_CCGR7 |= CCM_CCGR7_FLEXSPI2(CCM_CCGR_ON);
}

// uint16_t nemesis::adc_min[9];
// uint16_t nemesis::adc_max[9];

// float clamp(float n)
// {
//    if (n < 0.0)
//       return 0.0;
//    else if (n > 1.0)
//       return 1.0;
//    else
//       return n;
// }

// void writeIntIntoEEPROM(uint8_t address, uint16_t number)
// {
//    EEPROM.write(address, number >> 8);
//    EEPROM.write(address + 1, number & 0xFF);
// }

// uint16_t readIntFromEEPROM(uint8_t address)
// {
//    return (EEPROM.read(address) << 8) + EEPROM.read(address + 1);
// }

/* take measure 1V = x
 *  take measure 3V = y
 *
 *
 *
 */
// void nemesis::calibration()
// {

//    Serial.println("Turn all potentiometers to minimum position, then press t1");
//    while (digitalRead(switch_pin[0]) == HIGH)
//    {
//    }

//    Serial.println("deleting old calibration data");
//    for (int i = 0; i < EEPROM.length(); i++)
//    {
//       EEPROM.write(i, 0);
//    }

//    // store minimal value !!!RANGE IS INVERTED!!!
//    for (int i = 0; i < 4; i++)
//    {
//       adc_min[i] += analogRead(pot_pin[i]);
//       adc_min[i] = adc_min[i] >> 2;
//       writeIntIntoEEPROM(2 * i + 10, adc_min[i]);
//    }

//    Serial.println("Turn all potentiometers to maximum position, then press t2");
//    digitalWrite(led_pin[0], HIGH); // another led to indicate 2nd step in calibration
//    while (digitalRead(switch_pin[1]) == HIGH)
//    {
//    }

//    // store maximal value !!!RANGE IS INVERTED!!!
//    for (int i = 0; i < 4; i++)
//    {
//       adc_max[i] += analogRead(pot_pin[i]);
//       adc_max[i] = adc_max[i] >> 2;
//       writeIntIntoEEPROM(2 * i + 30, adc_max[i]);
//    }

//    EEPROM.write(0, 0xFF); // set EEPROM to 0xFF to check if calibration has been done

//    for (int i = 0; i < 9; i++)
//    {
//       Serial.print(adc_min[i]);
//       Serial.print("\t");
//    }
//    Serial.println();

//    for (int i = 0; i < 9; i++)
//    {
//       Serial.print(adc_max[i]);
//       Serial.print("\t");
//    }

//    Serial.println("to exit calibration press button");
//    while (digitalRead(switch_pin[2]) == HIGH)
//    {
//    }
//    digitalWrite(led_pin[0], LOW);
//    digitalWrite(led_pin[1], LOW);
// }

// void nemesis::loadCalibration()
// {
//    for (int i = 0; i < 9; i++)
//    {
//       adc_min[i] = readIntFromEEPROM(2 * i + 10);
//       adc_max[i] = readIntFromEEPROM(2 * i + 30);
//    }
// }

// uint16_t nemesis::getADC_min(int idx)
// {
//    return adc_min[idx];
// }

// uint16_t nemesis::getADC_max(int idx)
// {
//    return adc_max[idx];
// }

// void nemesis::setSampleRate(int freq)
// { // code by Frank B https://forum.pjrc.com/threads/38753-Discussion-about-a-simple-way-to-change-the-sample-rate?p=266346&viewfull=1#post266346
//    // PLL between 27*24 = 648MHz und 54*24=1296MHz
//    int n1 = 4; //SAI prescaler 4 => (n1*n2) = multiple of 4
//    int n2 = 1 + (24000000 * 27) / (freq * 256 * n1);
//    double C = ((double)freq * 256 * n1 * n2) / 24000000;
//    int c0 = C;
//    int c2 = 10000;
//    int c1 = C * c2 - (c0 * c2);
//    set_audioClock(c0, c1, c2, true);
//    //n1 = n1 / 2; //ai: Double Speed for TDM
//    CCM_CS1CDR = (CCM_CS1CDR & ~(CCM_CS1CDR_SAI1_CLK_PRED_MASK | CCM_CS1CDR_SAI1_CLK_PODF_MASK)) | CCM_CS1CDR_SAI1_CLK_PRED(n1 - 1) // &0x07
//                 | CCM_CS1CDR_SAI1_CLK_PODF(n2 - 1);                                                                                // &0x3f
// }
