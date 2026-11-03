//
// Library for the programmable Eurorack DSP Module NEMESIS
//
// nemesis_program.h
//
// Base class of every FAUST program compiled for the module. The FAUST
// architecture file (software/faust/nemesis_arch.cpp) derives from it, so
// the library can patch, control and (later) switch programs without
// knowing their generated class.
//
// Copyright © 2021 Arev Imer if not mentioned elsewise
// released under GNU GPL v3
//

#ifndef nemesis_program_h
#define nemesis_program_h

#include <AudioStream.h>
#include <string>
#include <vector>

namespace nemesis
{
   // output calibration, applied by the architecture file to every sample:
   // corrected = sample * scale + offset
   void outputCorrection(int output, float &scale, float &offset);
   // input polarity, applied by the architecture file to every input sample
   float inputScale(int input);

   // a FAUST parameter: driven by a hardware input (knob or CV jack), or a
   // setting changed in the menu when input is -1
   struct Control
   {
      float *zone;        // FAUST parameter memory
      float min, max;     // parameter range, input 0..1 is scaled to it
      float step;         // FAUST step
      int input;          // hardware input index (see nemesis::inputName()), -1 for a setting
      uint32_t cvMask;    // CV inputs added to it (bit per input index)
      bool gate;          // buttons/checkboxes: input > 0.5 -> 1, else 0
      const char *label;  // parameter label without the input prefix
      const char *menu;   // [style:menu{'name':value;..}] value names, or null
      float value;        // settings: the value set in the menu
      bool changed;       // settings: value still to be written to zone
   };

   // a FAUST bargraph shown on the home screen ([status:1]) or driving an LED ([led:1])
   struct Indicator
   {
      float *zone;
      float min, max;
      const char *label;
      const char *menu; // value names, see Control
      int led;          // LED index, -1 for a status value
   };

   class Program : public AudioStream
   {
   public:
      Program(unsigned char numStreamInputs, audio_block_t **inputQueue)
          : AudioStream(numStreamInputs, inputQueue) {}
      virtual ~Program() {}

      virtual const char *name() const = 0;
      // create the DSP and bind the controls; called when the program is
      // first selected, so unused programs take no memory
      virtual bool start() = 0;
      virtual bool started() const = 0;
      // number of FAUST audio channels (each uses an MSB and an LSB TDM slot)
      virtual int numAudioInputs() const = 0;
      virtual int numAudioOutputs() const = 0;

      virtual void setParamValue(const std::string &path, float value) = 0;
      virtual float getParamValue(const std::string &path) = 0;
      virtual int getParamsCount() = 0;
      virtual std::string getParamAddress(int index) = 0;
      // diagnostics, e.g. the note of every voice of a polyphonic program
      virtual void printStatus(Print &out) {}

      // bound parameters and settings
      const std::vector<Control> &controls() const { return fControls; }
      std::vector<Control> &controls() { return fControls; }
      const std::vector<Indicator> &indicators() const { return fIndicators; }

      // CV routing: the control a CV input is added to, -1 for none. The
      // default comes from the [cv:..] metadata, settings files change it.
      static constexpr int MAX_INPUTS = 16;
      int route(int cvInput) const;
      void setRoute(int cvInput, int control);
      int controlIndex(const char *label) const;
      bool settingsLoaded = false;

      // called by the architecture file while the FAUST UI is built:
      // declareControl() for each [key:value] metadata of a parameter,
      // then bindControl() for the parameter itself
      enum Kind { SLIDER, BUTTON, CHECKBOX, BARGRAPH };
      void declareControl(const char *key, const char *value);
      void bindControl(const char *label, float *zone, float init, float min, float max, float step, Kind kind);

   private:
      void updateMasks();

      std::vector<Control> fControls;
      std::vector<Indicator> fIndicators;
      bool fPendingStatus = false;
      int fPendingLed = -1;
      int8_t fRoute[MAX_INPUTS] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
      const char *fPendingInput = nullptr;
      const char *fPendingCV = nullptr;
      const char *fPendingMenu = nullptr;
      bool fPendingHidden = false;
      bool fPendingForRevision = false;
   };
}

#endif
