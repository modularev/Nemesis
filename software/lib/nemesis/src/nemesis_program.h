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

   // a FAUST parameter driven by a hardware input
   struct Control
   {
      float *zone;       // FAUST parameter memory
      float min, max;    // parameter range, input 0..1 is scaled to it
      int input;         // hardware input index, see nemesis::inputName()
      bool gate;         // buttons/checkboxes: input > 0.5 -> 1, else 0
      const char *label; // parameter label without the input prefix
   };

   class Program : public AudioStream
   {
   public:
      Program(unsigned char numStreamInputs, audio_block_t **inputQueue)
          : AudioStream(numStreamInputs, inputQueue) {}
      virtual ~Program() {}

      virtual const char *name() const = 0;
      // number of FAUST audio channels (each uses an MSB and an LSB TDM slot)
      virtual int numAudioInputs() const = 0;
      virtual int numAudioOutputs() const = 0;

      virtual void setParamValue(const std::string &path, float value) = 0;
      virtual float getParamValue(const std::string &path) = 0;
      virtual int getParamsCount() = 0;
      virtual std::string getParamAddress(int index) = 0;

      const std::vector<Control> &controls() const { return fControls; }

      // called by the architecture file while the FAUST UI is built:
      // declareControl() for each [key:value] metadata of a parameter,
      // then bindControl() for the parameter itself
      void declareControl(const char *key, const char *value);
      void bindControl(const char *label, float *zone, float min, float max, bool gate);

   private:
      std::vector<Control> fControls;
      const char *fPendingInput = nullptr;
      bool fPendingForRevision = false;
   };
}

#endif
