#include <SerialFlash.h>
#include <SD.h>
#include <SPI.h>
#include <Wire.h>
//#include <elapsedMillis.h>
#include "nemesis_hw.h"
#include <utility/imxrt_hw.h>
#include "mono_voice.h"
#include <typeinfo>
#include <list>
#include <memory>

//class audioObjects : public mono_voice{};

// Audio Connections
//
AudioInputTDM audio_in;
AudioOutputTDM audio_out;
std::list<AudioConnection*> patch_cords;

std::vector<abstractAudioClass*> faust_objects;
mono_voice synthA;
abstractAudioClass *AudioObject[] = {&synthA};

//mono_voice faustObj;

// void initAudioObjects(){
//    for (unsigned int i = 0; i < faust_objects.size(); i++){
//       faust_objects.emplace_back(AudioObjects[i]);
//    }
// }


void removeConnections(){
   for(auto cord : patch_cords){
      delete cord;
      patch_cords.clear();
   }
}

int activePatch = 0;

void addConnections(){
   int inputs = 2;
   int outputs = 2;   
   removeConnections();
   for(int i = 0 ; i < inputs ; i++){
      patch_cords.push_back(new AudioConnection(audio_in, i, AudioObject[activePatch], i));
   }
   for(int i = 0 ; i < outputs ; i++){
      patch_cords.push_back(new AudioConnection(AudioObject[activePatch], i, audio_out, i));
   }
}

//mono_voice faustObj;

// #ifdef HIGHRES
// // AudioConnection Lin_MSB(audio_in, 0, faustObj, 0);
// // AudioConnection Lin_LSB(audio_in, 1, faustObj, 1);
// // AudioConnection Rin_MSB(audio_in, 2, faustObj, 2);
// // AudioConnection Rin_LSB(audio_in, 3, faustObj, 3);
// AudioConnection Lout_MSB(faustObj, 0, audio_out, 0);
// AudioConnection Lout_LSB(faustObj, 1, audio_out, 1);
// AudioConnection Rout_MSB(faustObj, 2, audio_out, 2);
// AudioConnection Rout_LSB(faustObj, 3, audio_out, 3);
// // AudioConnection ch3out_MSB(faustObj, 4, audio_out, 4);
// // AudioConnection ch3out_LSB(faustObj, 5, audio_out, 5);
// // AudioConnection ch4out_MSB(faustObj, 6, audio_out, 6);
// // AudioConnection ch4out_LSB(faustObj, 7, audio_out, 7);
// #else
// // AudioConnection Lin_MSB(audio_in, 0, faustObj, 0);
// // AudioConnection Rin_MSB(audio_in, 2, faustObj, 1);
// AudioConnection Lout_MSB(faustObj, 0, audio_out, 0);
// AudioConnection Rout_MSB(faustObj, 1, audio_out, 2);
// #endif

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



std::vector<std::pair<std::string, int>> param;

void pathFinder()
{
   for (int i = 0; i < 11; i++)
   {
      for (int p = 0; p < AudioObject[activePatch].getParamsCount(); ++p)
      {
         size_t found = AudioObject[activePatch].getParamAddress(p).find(hwParams[i]);
         if (found != std::string::npos)
         {
            param.emplace_back(std::make_pair(AudioObject[activePatch].getParamAddress(p), i));
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
   codec.write(0x03, 0xF4 | 0x01);
   codec.write(0x05, 0x00);
   codec.filterFreeze();
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
      AudioObject[activePatch].setParamValue(param[p].first, returnVal(param[p].second));

   }
   //Serial.println(AudioProcessorUsage());
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