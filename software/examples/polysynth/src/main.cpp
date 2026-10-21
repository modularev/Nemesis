#include <SerialFlash.h>
#include <SD.h>
#include <SPI.h>
#include <Wire.h>
//#include <elapsedMillis.h>
#include "nemesis_hw.h"
#include <utility/imxrt_hw.h>
#include "superosc.h"
#include <typeinfo>

// Audio Connections
//
AudioInputTDM ADC;
AudioOutputTDM DAC;
superosc faustObj;

#ifdef HIGHRES
// AudioConnection In1(ADC, 0, faustObj, 0);
// AudioConnection In2(ADC, 1, faustObj, 1);
// AudioConnection In3(ADC, 2, faustObj, 2);
// AudioConnection In4(ADC, 3, faustObj, 3);
// AudioConnection In5(ADC, 4, faustObj, 4);
// AudioConnection In6(ADC, 5, faustObj, 5);
// AudioConnection In7(ADC, 6, faustObj, 6);
// AudioConnection In8(ADC, 7, faustObj, 7);
// AudioConnection In9(ADC, 8, faustObj, 8);
// AudioConnection In10(ADC, 9, faustObj, 9);
// AudioConnection In11(ADC, 10, faustObj, 10);
// AudioConnection In12(ADC, 11, faustObj, 11);
// AudioConnection In13(ADC, 12, faustObj, 12);
// AudioConnection In14(ADC, 13, faustObj, 13);
// AudioConnection In15(ADC, 14, faustObj, 14);
// AudioConnection In16(ADC, 15, faustObj, 15);
AudioConnection Out1(faustObj, 0, DAC, 0);
AudioConnection Out2(faustObj, 1, DAC, 1);
AudioConnection Out3(faustObj, 2, DAC, 2);
AudioConnection Out4(faustObj, 3, DAC, 3);
// AudioConnection Out5(faustObj, 4, DAC, 4);
// AudioConnection Out6(faustObj, 5, DAC, 5);
// AudioConnection Out7(faustObj, 6, DAC, 6);
// AudioConnection Out8(faustObj, 7, DAC, 7);
// AudioConnection Out9(faustObj, 8, DAC, 8);
// AudioConnection Out10(faustObj, 9, DAC, 9);
// AudioConnection Out11(faustObj, 10, DAC, 10);
// AudioConnection Out12(faustObj, 11, DAC, 11);
// AudioConnection Out13(faustObj, 12, DAC, 12);
// AudioConnection Out14(faustObj, 13, DAC, 13);
// AudioConnection Out15(faustObj, 14, DAC, 14);
// AudioConnection Out16(faustObj, 15, DAC, 15);
// AudioConnection ch3out_MSB(faustObj, 4, audio_out, 4);
// AudioConnection ch3out_LSB(faustObj, 5, audio_out, 5);
// AudioConnection ch4out_MSB(faustObj, 6, audio_out, 6);
// AudioConnection ch4out_LSB(faustObj, 7, audio_out, 7);
#else
// AudioConnection Lin_MSB(audio_in, 0, faustObj, 0);
// AudioConnection Rin_MSB(audio_in, 2, faustObj, 1);
AudioConnection Lout_MSB(faustObj, 0, audio_out, 0);
AudioConnection Rout_MSB(faustObj, 1, audio_out, 2);
#endif

//
// Parameters from Faust
//

// #ifdef MIDICTRL
// const char *polyAddress = "/Polyphonic/Voices/";
// #endif

using namespace nemesis;

int param_count = 0;
std::string hwParams[11] = {
    "CV_X",
    "CV_Y",
    "CV_Z",
    "CV_A",
    "CV_B",
    "CV_C",
    "CV_D",
    "POT_A",
    "POT_B",
    "POT_C",
    "POT_D",
};

extern unsigned long _heap_end;
extern char *__brkval;
uint32_t FreeMem(){ // for Teensy 4.1
  char* p = (char*) malloc(100000); // size should be quite big, to avoid allocating fragment!
  free(p);
  return (char *)&_heap_end - p; // __brkval;
}

float returnVal(int hw_num)
{
   switch (hw_num)
   {
   case 0:
      return cv_val[0];
   case 1:
      return cv_val[1];
   case 2:
      return cv_val[2];
   case 3:
      return cv_val[3];
   case 4:
      return cv_val[4];
   case 5:
      return cv_val[5];
   case 6:
      return cv_val[6];
   case 7:
      return pot_val[0];
   case 8:
      return pot_val[1];
   case 9:
      return pot_val[2];
   case 10:
      return pot_val[3];
   default:
      return 0;
   }
}

#include <vector>

std::vector<std::pair<std::string, int>> param;

void pathFinder()
{
   for (int i = 0; i < 11; i++)
   {
      for (int p = 0; p < faustObj.getParamsCount(); ++p)
      {
         size_t found = faustObj.getParamAddress(p).find(hwParams[i]);
         if (found != std::string::npos)
         {
            param.emplace_back(std::make_pair(faustObj.getParamAddress(p), i));
            break;
         }
      }
   }
   param_count = param.size();
}

void getParamArgs()
{

   for (int p = 0; p < param_count; ++p)
   {
      Serial.print(param[p].first.c_str());
      Serial.print("\t");
      Serial.println(param[p].second);
   }
}

void setup()
{
   Serial.begin(19200);
   setPSRAMspeed();

   pinMode(led_pin[0], OUTPUT);
   digitalWrite(led_pin[0], HIGH);
   pinMode(9, OUTPUT);

   pinMode(led_pin[1], OUTPUT);
   digitalWrite(led_pin[1], HIGH);

   display.begin(SSD1306_SWITCHCAPVCC, 0x3C);

   display.clearDisplay();
   display.setTextSize(1);
   display.setTextColor(WHITE);
   display.setFont(&SourceCodePro_ExtraLight6pt7b);
   display.setCursor(0, 7);
   display.print(F("say hello to"));
   display.display();

   display.setCursor(0, 31);
   display.setFont(&SourceCodePro_ExtraLightItalic14pt7b);
   display.print(F(">NEMESIS"));
   display.display();
   display.setFont(&SourceCodePro_ExtraLight6pt7b);
   delay(2000);

   for (int i = 0; i < POT_NUM; i++)
   {
      pinMode(pot_pin[i], INPUT_DISABLE);
   }

   // Setup both adc modules on teensy
   adc->adc0->setAveraging(ADC_AVERAGING);                          // set number of averages
   adc->adc0->setResolution(ADC_RES);                               // set bits of resolution
   adc->adc0->setConversionSpeed(ADC_CONVERSION_SPEED::MED_SPEED);  // change the conversion speed
   adc->adc0->setSamplingSpeed(ADC_SAMPLING_SPEED::VERY_LOW_SPEED); // change the sampling speed
   // adc->adc0->recalibrate();

   adc->adc1->setAveraging(ADC_AVERAGING);                          // set number of averages
   adc->adc1->setResolution(ADC_RES);                               // set bits of resolution
   adc->adc1->setConversionSpeed(ADC_CONVERSION_SPEED::MED_SPEED);  // change the conversion speed
   adc->adc1->setSamplingSpeed(ADC_SAMPLING_SPEED::VERY_LOW_SPEED); // change the sampling speed
   // adc->adc1->recalibrate();

   // for (int i = 0; i < 4; i++)
   // {
   //    pot_adc[i].init(pot_pin[i], TB_US, 500);
   //    pot_adc[i].enable();
   // }

   initADC();

   // POT_ADC_TIMER.begin(scan_POT, 1000); //
   // POT_ADC_TIMER.priority(230);

   // CV_ADC_TIMER.begin(scan_CV, 100); // 25kHz
   // CV_ADC_TIMER.priority(230);

   // OLED_REFRESH.begin(displayadc, 20000); // 50Hz
   // OLED_REFRESH.priority(255);

   digitalWrite(9, HIGH);
   AudioMemory(64); // 832 max
   codec.enable();
   codecWrite(0x03, 0xF4 | 0x01);
   codecWrite(0x05, 0x00);
   //codec.filterFreeze();
   codec.volume(1.);
   // init_POT();
   // while (!Serial)
   // {
   //    ;
   // }
   pathFinder();
   getParamArgs();
}

void update()
{
   for (int p = 0; p < param_count; ++p)
   {
      //delay(100);
      // Serial.print(returnVal(param[p].second));
      // Serial.print("\t ");
      faustObj.setParamValue(param[p].first, returnVal(param[p].second));

   }
   Serial.println(AudioProcessorUsageMax());
}
int i = 0;
void loop()
{
   scan_POT();
   scan_CV();
   update();

   if (i < 500)
      i++;
   else
   {
      displayadc();
      i = 0;
   }
   // delay(1);
}