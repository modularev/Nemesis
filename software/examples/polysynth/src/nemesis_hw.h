//
// Library for the programmable Eurorack DSP Module NEMESIS v0.2
//
// src/nemesis_hw.h
//
// Copyright © 2021 Arev Imer if not mentioned elsewise
// released under GNU GPL v3
//

#ifndef nemesis_hw_h
#define nemesis_hw_h

#include <Arduino.h>
#include <Audio.h>
#include <EEPROM.h>
#include <array>
#include <SPI.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_I2CDevice.h>
#include <Adafruit_SSD1306.h>
#include <SourceCodeProItalic14.h>
#include <SourceCodePro6.h>
#include <ADC.h>
#include <ADC_util.h>

#define INT16_DIV 1.525878906e-5
#define INT14_DIV 6.10388817676860e-5
#define INT12_DIV 2.44200244200244e-4
#define INT9_DIV 1.953125e-3
#define INT8_DIV 3.90625e-3

#define CW(N) (0x80 + (N << 4) + 0x0C) // +0x04 channel commands for unipolar mode
#define CV_NUM 7
const uint8_t adc_chan_word[CV_NUM] = {CW(0), CW(4), CW(1), CW(5), CW(2), CW(3), CW(6)}; // last two channels swapped on v0.2

// LTC1867 Single-Ended Channel Address
#define LTC1867_CH0 0x80
#define LTC1867_CH1 0xC0
#define LTC1867_CH2 0x90
#define LTC1867_CH3 0xD0
#define LTC1867_CH4 0xA0
#define LTC1867_CH5 0xE0
#define LTC1867_CH6 0xB0
#define LTC1867_CH7 0xF0

// LTC1867 Sleep / Unipolar/Bipolar config bits
#define LTC1867_SLEEP_MODE 0x02
#define LTC1867_EXIT_SLEEP_MODE 0x00
#define LTC1867_UNIPOLAR_MODE 0x04 // Bitwise OR with channel commands for unipolar mode
#define LTC1867_BIPOLAR_MODE 0x00

#define POT_NUM 4
constexpr int pot_pin[POT_NUM] = {A14, A15, A16, A17};

constexpr int led_pin[2] = {33, 34};
constexpr int switch_pin[2] = {35, 36};

float clamp(float n);
uint16_t readIntFromEEPROM(uint8_t address);
void writeIntIntoEEPROM(uint8_t address, uint16_t number);

ADC *adc = new ADC();

// Setup OLED
Adafruit_SSD1306 display(128, 32, &Wire, -1);


namespace nemesis
{
   IntervalTimer POT_ADC_TIMER;
   IntervalTimer CV_ADC_TIMER;
   IntervalTimer OLED_REFRESH;

   // static bool init(void);
   // static void calibration();
   // static void loadCalibration(void);

   // setup AudioControlCS42448 as the main codec
   static AudioControlCS42448 codec;


   // get 16 bit ADC value from last adc_command, send adc_command for next reading
   uint16_t getADC_raw(int next_chan)
   {
      // digitalWrite(PIN_SPI_SS, HIGH);
      digitalWriteFast(PIN_SPI_SS, LOW);
      uint16_t _raw_data = 0;

      SPI.beginTransaction(SPISettings(20000000, MSBFIRST, SPI_MODE0));
      // digitalWrite(PIN_SPI_SS, HIGH);
      // digitalWriteFast(PIN_SPI_SS, LOW);
      _raw_data = SPI.transfer16((adc_chan_word[next_chan]) << 8);
      digitalWriteFast(PIN_SPI_SS, HIGH);
      SPI.endTransaction();
      return _raw_data;
   }

   // start SPI and initialize LTC1867 with channel address 0 and unipolar mode
   void initADC(void)
   {
      pinMode(PIN_SPI_SS, OUTPUT);
      // digitalWrite(PIN_SPI_SS, HIGH);
      SPI.begin();
      getADC_raw(0); // dummy for next conversion
   }

   // POT XYZ: -> calibration not neccessary
   // CV A-D: 0V - 8.333V
   double scale_ADC(uint16_t raw_val)
   {
      return (double)((0x3FFF - (raw_val >> 2)) & 0x3FFF) * INT14_DIV;
   }

   
   #define LTC_SMOO 0.5f
   SPISettings LTC_SPI(8000000, MSBFIRST, SPI_MODE0);
   int next_cv_index = 0;
   uint16_t _raw_cv[CV_NUM];
   float cv_val[7];
   
   void scan_CV()
   {
      digitalWriteFast(PIN_SPI_SS, LOW);

      int current_chan = next_cv_index++;
      if(next_cv_index >= CV_NUM) next_cv_index = 0;

      SPI.beginTransaction(LTC_SPI);
      _raw_cv[current_chan] = SPI.transfer16((adc_chan_word[next_cv_index]) << 8);
      
      // trigger next ADC conversion
      digitalWriteFast(PIN_SPI_SS, HIGH);
      SPI.endTransaction();

      cv_val[current_chan] = LTC_SMOO * scale_ADC(_raw_cv[current_chan]) + (1 - LTC_SMOO) * cv_val[current_chan];

   }

#define SMOO_FACTOR 0.8f
#define ADC_AVERAGING 16
#define ADC_RES 12
#define WINDOW_SIZE 16

   float pot_val[POT_NUM] = {0, 0, 0, 0};
   int pot_smoo[POT_NUM] = {0, 0, 0, 0};
   uint16_t pot_raw[POT_NUM] = {0, 0, 0, 0};
   int pot_sum[POT_NUM] = {0, 0, 0, 0};
   int pot_readings[POT_NUM][WINDOW_SIZE];
   int pot_index[POT_NUM] = {0, 0, 0, 0};
   int current_pot = 0;

   void scan_POT()
   {
      pot_sum[current_pot] = pot_sum[current_pot] - pot_readings[current_pot][pot_index[current_pot]];
      pot_raw[current_pot] = adc->analogRead(pot_pin[current_pot]); // * INT9_DIV;      // read sensor value
      pot_readings[current_pot][pot_index[current_pot]] = pot_raw[current_pot];
      pot_sum[current_pot] = pot_sum[current_pot] + pot_readings[current_pot][pot_index[current_pot]];
      
      pot_index[current_pot]++;
      if (pot_index[current_pot] >= WINDOW_SIZE)
         pot_index[current_pot] = 0;
      
      pot_smoo[current_pot] = (pot_sum[current_pot] / WINDOW_SIZE);
      pot_val[current_pot] = SMOO_FACTOR * ((float)pot_smoo[current_pot] * INT12_DIV) + (1 - SMOO_FACTOR) * pot_val[current_pot];
      
      current_pot++;
      if (current_pot >= POT_NUM)
         current_pot = 0;
   }

   void displayadc()
   {
      display.clearDisplay();
      display.setCursor(0, 7);
      // cv_val[0] = scale_ADC(getADC_raw(LTC1867_CH1));
      // cv_val[1] = scale_ADC(getADC_raw(LTC1867_CH0));
      display.setCursor(0, 7);
      //cv_val[0] = scale_ADC(getADC_raw(1));
      display.println(cv_val[0], 3);
      display.setCursor(0, 17);
      //cv_val[1] = scale_ADC(getADC_raw(2));
      display.println(cv_val[1]);
      display.setCursor(0, 27);
      //cv_val[2] = scale_ADC(getADC_raw(3));
      display.println(cv_val[2]);
      display.setCursor(32, 7);
      //cv_val[3] = scale_ADC(getADC_raw(4));
      display.println(cv_val[3]);
      display.setCursor(32, 17);
      //cv_val[4] = scale_ADC(getADC_raw(5));
      display.println(cv_val[4]);
      display.setCursor(32, 27);
      //cv_val[5] = scale_ADC(getADC_raw(6));
      display.println(cv_val[5]);
      display.setCursor(64, 7);
      //cv_val[6] = scale_ADC(getADC_raw(0));
      display.println(cv_val[6]);

      display.setCursor(64, 27);
      display.println(pot_val[0]);
      display.setCursor(96, 7);
      display.println(pot_val[1]);
      display.setCursor(96, 17);
      display.println(pot_val[2]);
      display.setCursor(96, 27);
      display.println(pot_val[3]);

      display.display();
   }
   // static uint16_t getADC_min(int idx);
   // static uint16_t getADC_max(int idx);

   // static uint16_t *adc_max;
   // static uint16_t *adc_min;
   // static uint16_t adc_min[9];
   // static uint16_t adc_max[9];

   void setPSRAMspeed(void)
   {
      // Set SPI2 to 180Mhz
      CCM_CCGR7 |= CCM_CCGR7_FLEXSPI2(CCM_CCGR_OFF);
      CCM_CBCMR = (CCM_CBCMR & ~(CCM_CBCMR_FLEXSPI2_PODF_MASK | CCM_CBCMR_FLEXSPI2_CLK_SEL_MASK)) | CCM_CBCMR_FLEXSPI2_PODF(3) // 4 and 2 133MHz
                  | CCM_CBCMR_FLEXSPI2_CLK_SEL(1);
      CCM_CCGR7 |= CCM_CCGR7_FLEXSPI2(CCM_CCGR_ON);
   }
};

#endif
