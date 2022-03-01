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

#define INT16_DIV 1.52587890625e-5
#define INT9_DIV 1.953125e-3

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

constexpr int pot_pin[4] = {A14, A15, A16, A17};

constexpr int led_pin[2] = {33, 34};
constexpr int switch_pin[2] = {35, 36};

float clamp(float n);
uint16_t readIntFromEEPROM(uint8_t address);
void writeIntIntoEEPROM(uint8_t address, uint16_t number);

class nemesis
{

public:
   static bool init(void);
   // static void calibration();
   static void loadCalibration(void);

   // AudioControlCS42448 from the Audiolibrary
   static AudioControlCS42448 codec;

   // Adafruit Library to control the OLED
   // static Adafruit_SSD1306 display;

   // start SPI and initialize LTC1867 with channel address 0 and unipolar mode
   static void initADC(void);

   // get 16 bit ADC value from last adc_command, send adc_command for next reading
   static uint16_t getADC_raw(uint8_t adc_command);

   // POT XYZ: -> calibration not neccessary
   // CV A-D: 0V - 8.333V
   static float scale_ADC(uint16_t raw_val);

   static void scan_CV(void);
   static void scan_POT(void);

   // static uint16_t getADC_min(int idx);
   // static uint16_t getADC_max(int idx);

   static float cv_val[7];
   static float pot_val[4];

private:
   // static uint16_t *adc_max;
   // static uint16_t *adc_min;
   // static uint16_t adc_min[9];
   // static uint16_t adc_max[9];

   static void setPSRAMspeed(void); // void?
};

#endif
