// 
// Library for the programmable Eurorack DSP Module NEMESIS
//
// src/nemesis.h
//
// Copyright © 2021 Arev Imer if not mentioned elsewise
// released under GNU GPL v3
//

#ifndef nemesis_h
#define nemesis_h

#include <Arduino.h>
#include <EEPROM.h>
#include <array>

constexpr int analog_pin[9] = {
    A0, A1, A2, A10, A11, A12, A13, A14, A15};

constexpr int led_pin[3] = {35, 36, 37};
constexpr int switch_pin[3] = {28, 29, 30};


float clamp(float n);
uint16_t readIntFromEEPROM(uint8_t address);
void writeIntIntoEEPROM(uint8_t address, uint16_t number);


class nemesis
{

public:

   static bool init(void);
   static void setSampleRate(int freq);
   static void calibration(void);
   static void loadCalibration(void);
   static uint16_t getADC_min(int idx);
   static uint16_t getADC_max(int idx);

private:
   // static uint16_t *adc_max;
   // static uint16_t *adc_min; 
   static uint16_t adc_min[9];
   static uint16_t adc_max[9];
   static void setPSRAMspeed(void);
};


#endif
