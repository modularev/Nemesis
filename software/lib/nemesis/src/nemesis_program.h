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
#include <math.h>
#include <string>
#include <vector>

#include "nemesis_output.h"

// trace program switching over USB serial: build with -D NEMESIS_DEBUG_SWITCH
#ifdef NEMESIS_DEBUG_SWITCH
#define NEMESIS_TRACE(...) do { Serial.printf(__VA_ARGS__); Serial.flush(); } while (0)
#else
#define NEMESIS_TRACE(...) do { } while (0)
#endif

namespace nemesis
{
   // output calibration, applied by the architecture file to every sample:
   // corrected = sample * scale + offset
   void outputCorrection(int output, float &scale, float &offset);
   // input polarity, applied by the architecture file to every input sample
   float inputScale(int input);

   // fast RAM for the delay lines of the running program: a fixed area in
   // RAM1, filled in order and emptied as a whole when the program stops
   // (no fragmentation); null when a buffer doesn't fit
   void *arenaAllocate(size_t size);
   bool arenaOwns(const void *p);
   void arenaReset();

   // MIDI input (MIDI jack and USB MIDI), read in the background: channel
   // messages and real time messages for the program's MIDI handler
   struct MidiMessage
   {
      uint8_t status, data1, data2;
   };
   bool midiRead(MidiMessage &message);
   int midiChannel(); // system setting: 0 omni, 1..16
   void printMidiStatus(Print &out);

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
      bool log;           // [scale:log]: input 0..1 maps exponentially from min to max
      const char *unit;   // [unit:Hz], or null
      bool volts;         // [volts:1]: the input in volts instead of 0..1 (V/Oct)

      // parameter value <-> position 0..1 of the knob
      float fromPosition(float v) const
      {
         return log ? min * powf(max / min, v) : min + v * (max - min);
      }
      float position(float x) const
      {
         if (log)
            return logf(x / min) / logf(max / min);
         return (max != min) ? (x - min) / (max - min) : 0.0f;
      }
   };

   // a FAUST bargraph shown on the home screen ([status:1]) or driving an LED ([led:1])
   struct Indicator
   {
      float *zone;
      float min, max;
      const char *label;
      const char *menu; // value names, see Control
      int led;          // LED index, -1 for a status value
      const char *unit; // [unit:..], or null
      bool limit;       // [limit:1]: gain of a limiter, 0..1 (home screen shows it in dB)
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
      // frees the DSP and its memory when another program is selected, so the
      // running program gets the fast internal RAM; settings values and CV
      // routing are kept and restored by the next start()
      virtual void stop() {}
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

      // advances the transport and writes it to the [transport:..]
      // parameters, called by the architecture file before every block
      void beginBlock();
      // [mono:note|gate|velocity] parameters: monophonic MIDI, last note
      // priority, the note stays after note off (drones); called by the
      // architecture file for every MIDI message
      void handleMidi(const MidiMessage &message);
      // output clipping, counted by the architecture file
      void countClip(int output, uint32_t samples);
      uint32_t clipCount(int output) const { return (output >= 0 && output < 8) ? fClips[output] : 0; }
      uint32_t lastClipMillis() const { return fLastClip; }
      bool usesTransport() const { return !fTransport.empty(); }

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
      // global metadata of the program ('declare output "clip";',
      // 'declare audio_outputs "1-6";', see nemesis_output.h)
      void declareProgram(const char *key, const char *value);
      // adds the "output" setting; called after the FAUST UI is bound
      void bindOutputSetting();

      // output stage of the audio outputs (nemesis_output.h), applied by
      // the architecture file
      int outputMode() const { return (int)fOutputMode; }
      bool isAudioOutput(int output) const { return output >= 0 && output < 32 && (fAudioOutputs >> output) & 1; }

   protected:
      // around stop() and start(): keep the settings and routing, drop the
      // bindings (their zones are freed), and put them back after binding again
      void releaseBindings();
      void restoreBindings();

   private:
      void updateMasks();

      std::vector<Control> fControls;
      std::vector<Indicator> fIndicators;
      struct TransportZone
      {
         float *zone;
         int what; // TRANSPORT_* in nemesis_program.cpp
      };
      std::vector<TransportZone> fTransport;
      int fPendingTransport = -1;
      std::vector<TransportZone> fMono; // what: MONO_* in nemesis_program.cpp
      int fPendingMono = -1;
      bool fPendingVolts = false;
      uint8_t fNotes[16];
      int fNoteCount = 0;
      volatile uint32_t fClips[8] = {};
      std::vector<std::pair<const char *, float>> fKeptValues;
      int8_t fKeptRoute[MAX_INPUTS];
      bool fKept = false;
      volatile uint32_t fLastClip = 0;
      bool fPendingStatus = false;
      bool fPendingLimit = false;
      int fPendingLed = -1;
      int8_t fRoute[MAX_INPUTS] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
      const char *fPendingInput = nullptr;
      const char *fPendingCV = nullptr;
      const char *fPendingMenu = nullptr;
      bool fPendingLog = false;
      const char *fPendingUnit = nullptr;
      bool fPendingHidden = false;
      bool fPendingForRevision = false;
      float fOutputMode = output::SOFT;    // zone of the "output" setting
      int fOutputDefault = output::SOFT;
      uint32_t fAudioOutputs = output::DEFAULT_OUTPUTS;
   };
}

#endif
