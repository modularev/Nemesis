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
   // the transport's clock comes from audio IN 8 (clock source CV); that
   // input then reads silence in the program
   bool clockOnInput8();
   // testing: the running program's DSP is not computed (silence)
   extern volatile bool debugMute;
   // testing: the DSP is computed, its outputs are replaced by silence
   extern volatile bool debugSilent;

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
   class Program;
   int sampleRate(); // in use: 48000 or 96000 (nemesis.cpp)
   // from a program's start(), after its metadata, before its DSP is
   // initialized: sets the hardware to the sample rate the setting and the
   // program ask for, returns it (nemesis.cpp)
   int applySampleRate(const Program &program);
   // the outputs fade to outputTarget (0 or 1): 5 ms down, 10 ms up, applied
   // by the architecture file to every output sample (program switches,
   // block size and sample rate changes); outputGain is where it got
   extern volatile float outputTarget;
   extern volatile float outputGain;
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
      bool volts;         // [volts:1]: the input in volts instead of 0..1 (V/Oct), also routed CV
      bool listed;        // settings: shown in the settings list ([setting:0]: only a CV target)
      const char *name = nullptr; // [name:lag]: shown instead of the label (labels stay unique: saved settings)
      const char *cond = nullptr; // [if:sync] / [ifnot:sync]: listed only while that setting is on / off,
      int condLen = 0;            // [if:voice=3]: while it has that value (cond: the label's first condLen characters)
      float condValue = 0.0f;
      bool condEquals = false;
      bool condOn = true;
      // [edit:v{voice} pitch] on a knob: turning it sets the setting of that
      // label, {voice} replaced by the value of the setting "voice" (the knob
      // picks up when it moves, after a change of the target)
      const char *edit = nullptr;
      int editIndex = -1;
      float editSet = 0.0f;   // the value this edit knob last gave its target
      // knobs (rev2 POT A-D, X/Y/Z): after a stored value was loaded (start,
      // program switch, another edit target) the knob takes over only when
      // it reaches that value (catch); catchRef: its last position
      bool knob = false;
      bool caught = true;
      float catchRef = -1.0f;
      // edit knob turned with button 1 held: the knob position and every
      // page's value (as position) where the relative edit started
      static constexpr int MAX_PAGES = 8;
      float relRef = -1.0f;
      float relBase[MAX_PAGES];
      // [section:object]: settings list headers; [pager:1]: button 1 steps
      // this setting (e.g. the voice a program edits), [pagename:v{voice}
      // type] the setting whose value names the page on the home screen
      const char *section = nullptr;
      bool pager = false;
      const char *pageName = nullptr;
      // [action:randomize]: a menu (off / page / all) acted on when its edit
      // ends; [action:amount]: how far the randomization moves (0..1);
      // [random:0]: left out of the randomization
      int action = 0;
      bool randomizable = true;
      // [title:x]: a knob's popup shows the value name of setting "x" (e.g.
      // what an assignable knob controls) instead of its label
      const char *title = nullptr;

      const char *shown() const { return name ? name : label; }

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
      int activity = -1; // [activity:N]: page N's activity (the home screen strip, > half: lit)
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
      // updated by the Audio library (only the running program)
      void setActive(bool on) { active = on; }
      // number of FAUST audio channels (one 32 bit TDM slot each)
      virtual int numAudioInputs() const = 0;
      virtual int numAudioOutputs() const = 0;

      virtual void setParamValue(const std::string &path, float value) = 0;
      virtual float getParamValue(const std::string &path) = 0;
      virtual int getParamsCount() = 0;
      virtual std::string getParamAddress(int index) = 0;
      // diagnostics, e.g. the note of every voice of a polyphonic program
      virtual void printStatus(Print &out) {}

      // loop memory saved on the SD card (nemesis_loop.h): the DSP's large
      // delay lines (>= 256 kB, in allocation order) and the samples
      // computed since start(), which give their write position
      virtual int largeBuffers(float **buffers, uint32_t *lengths, int max) const { return 0; }
      virtual uint64_t samplesComputed() const { return 0; }
      // [loop:length] bargraph: length of the frozen loop in samples (0 while
      // recording), [loop:freeze] the freeze setting, [loop:restored] a
      // hidden slider that gets the length of a loop loaded at start
      float *loopLength() const { return fLoopLength; }
      float *loopFreeze() const { return fLoopFreeze; }
      float *loopRestored() const { return fLoopRestored; }

      // bound parameters and settings
      const std::vector<Control> &controls() const { return fControls; }
      std::vector<Control> &controls() { return fControls; }
      const std::vector<Indicator> &indicators() const { return fIndicators; }

      // advances the transport and writes it to the [transport:..]
      // parameters, called by the architecture file before every block
      void beginBlock();
      // [mono:note|gate|velocity] parameters: monophonic MIDI, last note
      // priority, the note stays after note off (drones); [mono:note1..4] and
      // [mono:gate1..4]: notes for up to 4 voices of a paraphonic program
      // (a new note takes the lowest free voice, else the oldest note is
      // stolen); [mono:rr1..4]: four pitches, a note pressed alone sets all
      // four, notes added while keys are held go to the next one in turn
      // (2, 3, 4, 1 ..) and stay; [mono:sp1..4]: four pitches spread over
      // the notes played together: one note all four, two notes 1+2 / 3+4,
      // three notes 1+2 / 3 / 4, four notes one each (the lowest note gets
      // the first ones), recalculated on every key press, lifting keeps it,
      // a note played alone starts anew; called by the architecture file for
      // every MIDI message.
      // FAUST's MIDI metadata, as FAUST's MidiUI maps it (the programs stay
      // standard FAUST): [midi:ctrl N], [midi:ctrl N C], [midi:keyon N],
      // [midi:keyoff N], [midi:key N], [midi:keypress N] (with an optional
      // channel C, 1-16), [midi:pgm], [midi:chanpress], [midi:pitchwheel]
      // (also pitchbend), [midi:start], [midi:stop], [midi:clock]; the
      // program's MIDI channel setting filters first. A setting or knob takes
      // the value as its own (menu, saved; a knob catches it like a loaded
      // value), other parameters get it written directly.
      void handleMidi(const MidiMessage &message);
      // a MIDI message changed a setting since the last call (from loop():
      // the settings are saved after a pause)
      bool takeMidiEdited()
      {
         bool edited = fMidiEdited;
         fMidiEdited = false;
         return edited;
      }
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
      // a setting's [if:]/[ifnot:] condition holds (always true without one)
      bool visible(const Control &c) const;
      // the setting is the one another setting's condition names
      bool isCondition(const Control &setting, const Control &c) const;
      // the setting an [edit:] knob changes now (or on page `page`), -1 for none
      int editTarget(const Control &knob, int page = -1) const;
      // the setting a label template names: "v{voice} type" -> "v3 type"
      // ({voice}: that setting's value, or page), -1 for none
      int templateTarget(const char *label, int page = -1) const;
      // [pager:1] setting, -1 for none; its value; the next page (button 1)
      int pagerIndex() const { return fPager; }
      int page() const;
      void nextPage();
      // [button:N] parameters: 1 while button N is held
      void setButtons(bool b1, bool b2);
      // randomizes the settings of one page (or all: page < 0) by the
      // [action:amount] setting; an [action:randomize] setting's edit ends
      void randomize(int page);
      void actionDone(Control &c);
      // ACTION_PRESET: [action:preset], a setting choosing a line of the
      // program's preset file /NEMESIS/<program>.PRE (nemesis_settings.cpp)
      enum { ACTION_RANDOMIZE = 1, ACTION_AMOUNT = 2, ACTION_ROLL = 3, ACTION_TARGET = 4, ACTION_PRESET = 5 };
      // rolling: every roll randomizes the [action:target] (the current page,
      // or all) and is kept, rollBack() returns to the one before; #0 is the
      // state before the first roll. The [action:roll] setting shows the
      // position; button 1 held + button 2 rolls the current page.
      static constexpr int MAX_ROLLS = 16;
      void roll(int page);
      void rollBack();
      int rollPosition() const { return fRollPos; }
      int rollTarget() const;
      uint32_t lastRollMillis() const { return fRollMillis; }
      bool canRoll() const;
      // a setting a roll leaves alone: an edit knob's target on that page
      // (the knob sets it), or one with a routed CV that carries a signal
      bool held(const Control &c, int page) const;
      // the value of a control as stored in the settings: settings, and the
      // knobs' last values (catch)
      static bool stored(const Control &c) { return (c.input < 0 || c.knob) && !c.edit && c.action != ACTION_ROLL; }
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
      // 'declare block_size "32";': the block size the program asks for
      // (system setting block size auto), 0 if it doesn't say
      int blockSizeHint() const { return fBlockSize; }
      // 'declare sample_rate "96000";' (system setting sample rate: auto), 0 if it doesn't say
      int sampleRateHint() const { return fSampleRate; }
      // 'declare cpu_mhz "600";': a lower processor clock for the program
      // (dense code from ITCM at 816 MHz browned out the module), 0: F_CPU
      int cpuMHz() const { return fCpuMHz; }
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
         // mono parameters only: [chan:N] channel base + N - 1 (0: the
         // program's channel), [note:N] only that note (-1: any), and the
         // notes held on that filter
         int chan = 0;
         int key = -1;
         int held = 0;
      };
      std::vector<TransportZone> fTransport;
      int fPendingTransport = -1;
      std::vector<TransportZone> fMono; // what: MONO_* in nemesis_program.cpp
      int fPendingMono = -1;
      int fPendingChan = 0;
      int fPendingKey = -1;
      void handleFilteredMidi(const MidiMessage &m, bool on, bool off);
      // [midi:..] parameters
      struct MidiZone
      {
         float *zone;
         float min, max;
         int control;  // index in fControls, or -1: written directly
         uint8_t type; // MIDI_* in nemesis_program.cpp
         uint8_t num;  // controller or note number
         uint8_t chan; // 1-16, 0: any
         bool log;
      };
      std::vector<MidiZone> fMidiZones;
      const char *fPendingMidi = nullptr;
      volatile bool fMidiEdited = false;
      void addMidiZone(const char *meta, float *zone, float min, float max);
      void setFromMidi(const MidiZone &z, float value);
      void handleMidiZones(const MidiMessage &m);
      bool fPendingVolts = false;
      bool fPendingUnlisted = false;
      const char *fPendingName = nullptr;
      const char *fPendingCond = nullptr;
      const char *fPendingEdit = nullptr;
      const char *fPendingTitle = nullptr;
      bool fPendingCondOn = true;
      uint8_t fNotes[16];
      int fNoteCount = 0;
      static constexpr int MAX_SLOTS = 4; // paraphonic voices
      uint8_t fSlotNote[MAX_SLOTS] = {255, 255, 255, 255}; // 255: no note yet (the program's default stays)
      bool fSlotOn[MAX_SLOTS] = {};
      uint32_t fSlotAge[MAX_SLOTS] = {}; // when the slot was last started or released
      uint32_t fSlotClock = 0;
      int fSlots = 0;                    // voices the program asks for
      uint8_t fRR[MAX_SLOTS] = {255, 255, 255, 255}; // round robin pitches, 255: none yet
      int fRRNext = 1;
      uint8_t fChord[MAX_SLOTS];         // spread: the notes played together, in order
      int fChordCount = 0;
      uint8_t fSpread[MAX_SLOTS] = {255, 255, 255, 255};
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
      const char *fPendingSection = nullptr;
      int fPendingAction = 0;
      bool fPendingRandom = true;
      const char *fPendingPageName = nullptr;
      bool fPendingPager = false;
      int fPendingButton = -1;
      int fPendingActivity = -1;
      int fPager = -1;
      std::vector<std::pair<float *, int>> fButtons; // zone, button 0/1
      std::vector<std::vector<float>> fRolls;        // values of the rollable settings
      int fRollPos = 0;
      uint32_t fRollMillis = 0;
      bool rollable(const Control &c) const;
      void snapshot(std::vector<float> &values) const;
      void restore(const std::vector<float> &values);
      void showRoll();
      const char *fPendingLoop = nullptr;
      float *fLoopLength = nullptr;
      float *fLoopFreeze = nullptr;
      float *fLoopRestored = nullptr;
      bool fPendingForRevision = false;
      float fOutputMode = output::SOFT;    // zone of the "output" setting
      int fOutputDefault = output::SOFT;
      uint32_t fAudioOutputs = output::DEFAULT_OUTPUTS;
      int fBlockSize = 0;
      int fCpuMHz = 0;
      int fSampleRate = 0;
   };
}

#endif
