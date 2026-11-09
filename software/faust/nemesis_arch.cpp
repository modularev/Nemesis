/************************************************************************
 IMPORTANT NOTE : this file contains two clearly delimited sections :
 the ARCHITECTURE section (in two parts) and the USER section. Each section
 is governed by its own copyright and license. Please check individually
 each section for license and copyright information.
 *************************************************************************/

/*******************BEGIN ARCHITECTURE SECTION (part 1/2)****************/

/************************************************************************
 FAUST Architecture File for the NEMESIS Eurorack DSP module
 based on architecture/teensy/teensy.cpp
 Copyright (C) 2019-2020 GRAME, Centre National de Creation Musicale &
 Aalborg University (Copenhagen, Denmark)
 Copyright (C) 2021 Arev Imer
 ---------------------------------------------------------------------
 This Architecture section is free software; you can redistribute it
 and/or modify it under the terms of the GNU General Public License
 as published by the Free Software Foundation; either version 3 of
 the License, or (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program; If not, see <http://www.gnu.org/licenses/>.

 EXCEPTION : As a special exception, you may create a larger work
 that contains this FAUST architecture section and distribute
 that work under terms of your choice, so long as this FAUST
 architecture section is not modified.

 ************************************************************************/

// Used by software/faust/faust_build.py, which compiles with
//   faust -i -mem -uim -cn NemesisAudio_dsp -a nemesis_arch.cpp
// replaces 'NemesisAudio' with the name of the .dsp file and prepends
//   MIDICTRL                 [midi:on] in 'declare options'
//   NVOICES <n>              [nvoices:n] in 'declare options'
//   NEMESIS_FAUST_GLOBALS    in exactly one program that uses MIDI
//   NEMESIS_EFFECT           polyphonic program with an 'effect' (compiled as
//                            NemesisAudio_effect, inserted below), and
//   NEMESIS_NUM_OUTPUTS      the effect's outputs

#include <string.h> // for memset

#include "NemesisAudio.h"
#include <nemesis_tdm.h>  // the TDM DMA buffers

// IMPORTANT: in order for MapUI to work, the teensy linker must be g++
#include "faust/gui/MapUI.h"
#include "faust/gui/UI.h"
#include "faust/gui/meta.h"
#include "faust/dsp/dsp.h"

// MIDI support
#ifdef MIDICTRL
#include "faust/gui/MidiUI.h"
#include "faust/midi/nemesis-serial-midi.h" // unique name: FAUST searches its install dirs before -A
#endif

// for polyphonic synths
#ifdef NVOICES
#include "faust/dsp/poly-dsp.h"
#endif

// we require macro declarations
#define FAUST_UIMACROS

// but we will ignore most of them
#define FAUST_ADDBUTTON(l,f)
#define FAUST_ADDCHECKBOX(l,f)
#define FAUST_ADDVERTICALSLIDER(l,f,i,a,b,s)
#define FAUST_ADDHORIZONTALSLIDER(l,f,i,a,b,s)
#define FAUST_ADDNUMENTRY(l,f,i,a,b,s)
#define FAUST_ADDVERTICALBARGRAPH(l,f,a,b)
#define FAUST_ADDHORIZONTALBARGRAPH(l,f,a,b)

/******************************************************************************
 *******************************************************************************

 VECTOR INTRINSICS

 *******************************************************************************
 *******************************************************************************/

<<includeIntrinsic>>

/********************END ARCHITECTURE SECTION (part 1/2)****************/

/**************************BEGIN USER SECTION **************************/

<<includeclass>>

// <<NEMESIS_EFFECT_CLASS>>

/***************************END USER SECTION ***************************/

/*******************BEGIN ARCHITECTURE SECTION (part 2/2)***************/

#ifndef NEMESIS_NUM_OUTPUTS
#define NEMESIS_NUM_OUTPUTS FAUST_OUTPUTS
#endif

// samples are 32 bit words in the TDM DMA buffers (24 bit audio, left justified)
#define MULT_32 2147483648.0f
#define DIV_32 4.656612873077392578125e-10f

#if defined(MIDICTRL) && defined(NEMESIS_FAUST_GLOBALS)
std::list<GUI*> GUI::fGuiList;
ztimedmap GUI::gTimedZoneMap;
#endif

// DSP memory: small allocations from the heap; delay lines up to
// NEMESIS_RAM_BUFFER_MAX from the library's RAM arena (fast, read on every
// sample: PSRAM reads cost a lot of CPU, e.g. mooSpace), the rest from the
// optional PSRAM (extmem_malloc() falls back to RAM without PSRAM).
// MIDI programs stay allocated (see stop()), so they don't use the arena,
// which is emptied whenever a program stops.
#ifndef NEMESIS_EXTMEM_THRESHOLD
#define NEMESIS_EXTMEM_THRESHOLD 1024
#endif
#ifndef NEMESIS_RAM_BUFFER_MAX
#define NEMESIS_RAM_BUFFER_MAX 32768
#endif

struct NemesisAudioMemory : public dsp_memory_manager {
    void* allocate(size_t size) override
    {
        if (size < NEMESIS_EXTMEM_THRESHOLD) return malloc(size);
#if !defined(NVOICES) && !defined(MIDICTRL)
        if (size <= NEMESIS_RAM_BUFFER_MAX) {
            void* p = nemesis::arenaAllocate(size);
            if (p) { NEMESIS_TRACE("  alloc %u arena\n", (unsigned)size); return p; }
        }
#endif
        NEMESIS_TRACE("  alloc %u psram\n", (unsigned)size);
        return extmem_malloc(size);
    }
    void destroy(void* ptr) override
    {
        if (nemesis::arenaOwns(ptr)) return;  // emptied as a whole in stop()
        extmem_free(ptr); // falls back to free(ptr) for addresses in RAM
    }
};

static NemesisAudioMemory gNemesisAudioMemory;

// binds parameters to hardware inputs, see software/lib/nemesis/src/nemesis_program.cpp
// only used while a program is constructed
struct NemesisAudioBindUI : public UI {
    nemesis::Program* fProgram;

    NemesisAudioBindUI(nemesis::Program* program) : fProgram(program) {}

    void openTabBox(const char* label) override {}
    void openHorizontalBox(const char* label) override {}
    void openVerticalBox(const char* label) override {}
    void closeBox() override {}

    void addButton(const char* label, FAUSTFLOAT* zone) override
    {
        fProgram->bindControl(label, zone, 0, 0, 1, 1, nemesis::Program::BUTTON);
    }
    void addCheckButton(const char* label, FAUSTFLOAT* zone) override
    {
        fProgram->bindControl(label, zone, 0, 0, 1, 1, nemesis::Program::CHECKBOX);
    }
    void addVerticalSlider(const char* label, FAUSTFLOAT* zone, FAUSTFLOAT init, FAUSTFLOAT min, FAUSTFLOAT max, FAUSTFLOAT step) override
    {
        fProgram->bindControl(label, zone, init, min, max, step, nemesis::Program::SLIDER);
    }
    void addHorizontalSlider(const char* label, FAUSTFLOAT* zone, FAUSTFLOAT init, FAUSTFLOAT min, FAUSTFLOAT max, FAUSTFLOAT step) override
    {
        fProgram->bindControl(label, zone, init, min, max, step, nemesis::Program::SLIDER);
    }
    void addNumEntry(const char* label, FAUSTFLOAT* zone, FAUSTFLOAT init, FAUSTFLOAT min, FAUSTFLOAT max, FAUSTFLOAT step) override
    {
        fProgram->bindControl(label, zone, init, min, max, step, nemesis::Program::SLIDER);
    }

    // outputs only show status values or drive LEDs
    void addHorizontalBargraph(const char* label, FAUSTFLOAT* zone, FAUSTFLOAT min, FAUSTFLOAT max) override
    {
        fProgram->bindControl(label, zone, 0, min, max, 0, nemesis::Program::BARGRAPH);
    }
    void addVerticalBargraph(const char* label, FAUSTFLOAT* zone, FAUSTFLOAT min, FAUSTFLOAT max) override
    {
        fProgram->bindControl(label, zone, 0, min, max, 0, nemesis::Program::BARGRAPH);
    }
    void addSoundfile(const char* label, const char* filename, Soundfile** sf_zone) override {}

    void declare(FAUSTFLOAT* zone, const char* key, const char* val) override
    {
        if (zone) fProgram->declareControl(key, val);
    }
};

// global metadata: 'declare output "clip";', 'declare audio_outputs "1-6";'
struct NemesisAudioMeta : public Meta {
    nemesis::Program* fProgram;
    NemesisAudioMeta(nemesis::Program* program) : fProgram(program) {}
    void declare(const char* key, const char* value) override { fProgram->declareProgram(key, value); }
};

// no Audio library inputs: the program reads the TDM DMA buffer (nemesis_tdm.h)
FLASHMEM NemesisAudio::NemesisAudio() : nemesis::Program(0, nullptr),
    fInChannel(nullptr), fOutChannel(nullptr), fUI(nullptr), fMIDIHandler(nullptr), fMIDIInterface(nullptr), fDSP(nullptr), fVoices(nullptr), fResets(0)
{
}

// creates the DSP (memory in PSRAM where possible), binds the controls and MIDI
FLASHMEM bool NemesisAudio::start()
{
    if (fDSP) return true;
#ifdef NVOICES
    mydsp_poly* poly;
#endif

    mydsp::fManager = &gNemesisAudioMemory;
    NEMESIS_TRACE("  classInit\n");
    mydsp::classInit(AUDIO_SAMPLE_RATE_EXACT);
#ifdef NVOICES
    // voices are cloned with mydsp::create(), so they use the memory manager too
    poly = new mydsp_poly(mydsp::create(), NVOICES, true, true);
    fVoices = poly;
#ifdef NEMESIS_EFFECT
    // the effect runs once after the voice mix, MIDI still goes to the voices
    NemesisAudio_effect::fManager = &gNemesisAudioMemory;
    NemesisAudio_effect::classInit(AUDIO_SAMPLE_RATE_EXACT);
    dsp* effect = NemesisAudio_effect::create();
    effect->instanceInit(AUDIO_SAMPLE_RATE_EXACT);
    fDSP = new dsp_poly_effect(poly, new dsp_sequencer(poly, effect, AUDIO_BLOCK_SAMPLES));
#else
    fDSP = poly;
#endif
#else
    fDSP = mydsp::create();
#endif
    // with -mem, init() is empty: classInit() above plus instanceInit()
    fDSP->instanceInit(AUDIO_SAMPLE_RATE_EXACT);

    NEMESIS_TRACE("  MapUI\n");
    fUI = new MapUI();
    fDSP->buildUserInterface(fUI);

    NEMESIS_TRACE("  bind\n");
    NemesisAudioBindUI bindUI(this);
    fDSP->buildUserInterface(&bindUI);
    NemesisAudioMeta meta(this);
    fDSP->metadata(&meta);
    bindOutputSetting();  // "output": clip/soft/tape/fold of the audio outputs

    fInChannel = new float*[FAUST_INPUTS];
    for (int i = 0; i < FAUST_INPUTS; i++) {
        fInChannel[i] = new float[AUDIO_BLOCK_SAMPLES];
    }
    fOutChannel = new float*[NEMESIS_NUM_OUTPUTS];
    for (int i = 0; i < NEMESIS_NUM_OUTPUTS; i++) {
        fOutChannel[i] = new float[AUDIO_BLOCK_SAMPLES];
    }

#ifdef MIDICTRL
    NEMESIS_TRACE("  MIDI\n");
    fMIDIHandler = new nemesis_midi();
    fMIDIInterface = new MidiUI(fMIDIHandler);
    // a polyphonic DSP registers itself for MIDI notes here (FAUST's poly-dsp.h)
    fDSP->buildUserInterface(fMIDIInterface);
    fMIDIInterface->run();
#endif
    NEMESIS_TRACE("  restore\n");
    restoreBindings();  // settings and CV routing from before a stop()
    fReady = true;
    return true;
}

FLASHMEM NemesisAudio::~NemesisAudio()
{
    stop();
}

// frees the DSP, its buffers and the MIDI handlers; start() builds them again
FLASHMEM void NemesisAudio::stop()
{
    if (!fDSP) return;
#if defined(NVOICES) || defined(MIDICTRL)
    // MIDI programs stay allocated (they are small): FAUST's poly DSP deletes
    // its voices with delete, which can't free memory from our memory manager,
    // and deleting a MidiUI while another program's MidiUI exists hangs in
    // FAUST's shared MIDI tables
    return;
#endif
    fReady = false;
    NEMESIS_TRACE("  release\n");
    releaseBindings();  // the parameter zones go away with the DSP

    for (int i = 0; i < FAUST_INPUTS; i++) {
        delete[] fInChannel[i];
    }
    delete[] fInChannel;
    fInChannel = nullptr;
    for (int i = 0; i < NEMESIS_NUM_OUTPUTS; i++) {
        delete[] fOutChannel[i];
    }
    delete[] fOutChannel;
    fOutChannel = nullptr;
    NEMESIS_TRACE("  delete MapUI\n");
    delete fUI;
    fUI = nullptr;
#ifdef NVOICES
    delete fDSP;
    fVoices = nullptr;
#else
    mydsp::destroy(fDSP);
#endif
    NEMESIS_TRACE("  classDestroy\n");
    fDSP = nullptr;
    mydsp::classDestroy();
    nemesis::arenaReset();  // this program's delay lines
#ifdef MIDICTRL
    // a poly DSP unregisters from the MidiUI, the MidiUI from the handler:
    // delete them in that order
    delete fMIDIInterface;
    fMIDIInterface = nullptr;
    delete fMIDIHandler;
    fMIDIHandler = nullptr;
#endif
}

void NemesisAudio::update(void)
{
    if (!fReady) return;  // not started, or start() is still allocating
    // samples in this block (set at runtime, at most the buffers' 128)
    const int n = nemesis::tdm::blockSize();
    // transport position for this block ([transport:..] parameters)
    beginBlock();
    // MIDI received in the background: mono note parameters, then FAUST's MIDI
    // handler; programs without MIDI only empty the queue, so old messages
    // don't reach the next program
    nemesis::MidiMessage message;
    while (nemesis::midiRead(message)) {
        handleMidi(message);
#ifdef MIDICTRL
        fMIDIHandler->handle(message);
#endif
    }
#ifdef MIDICTRL
    // MidiUI queues [midi:clock] toggles with a timestamp for a timed DSP,
    // which this architecture doesn't run: apply them now, else the queue
    // fills up and the clock never reaches the program
    for (auto& timed : GUI::gTimedZoneMap) {
        if (!timed.second) continue;  // operator[] in FAUST's modifyZone can leave empty entries
        DatedControl dated(0, 0);
        while (ringbuffer_read_space(timed.second) >= sizeof(DatedControl)) {
            ringbuffer_read(timed.second, (char*)&dated, sizeof(DatedControl));
            *timed.first = dated.fValue;
        }
    }
    // Synchronize all GUI controllers
    GUI::updateAllGuis();
#endif

    // inputs from the receive DMA buffer: one 32 bit word per channel and
    // frame (24 bit audio, left justified), see nemesis_tdm.h
    const int32_t* frames = nemesis::tdm::input();
    for (int channel = 0; channel < FAUST_INPUTS; channel++) {
        // IN 8 is the transport's clock: silent here
        if (!frames || channel >= nemesis::tdm::CHANNELS || (channel == 7 && nemesis::clockOnInput8())) {
            memset(fInChannel[channel], 0, n * sizeof(float));
            continue;
        }
        // input polarity of the module
        float scale = DIV_32 * nemesis::inputScale(channel);
        const int32_t* src = frames + channel;
        for (int i = 0; i < n; i++) {
            fInChannel[channel][i] = src[i * nemesis::tdm::CHANNELS] * scale;
        }
    }

    fDSP->compute(n, fInChannel, fOutChannel);

    // safety net: an unstable filter can leave inf/NaN in the DSP state, which
    // would silence the program for good, so clear the state and drop the block
    for (int channel = 0; channel < NEMESIS_NUM_OUTPUTS; channel++) {
        if (!std::isfinite(fOutChannel[channel][n - 1])) {
            fDSP->instanceClear();
            for (int c = 0; c < NEMESIS_NUM_OUTPUTS; c++) {
                memset(fOutChannel[c], 0, n * sizeof(float));
            }
            fResets++;
            break;
        }
    }

    // output stage (setting "output", see nemesis_output.h)
    int mode = outputMode();
    if (mode != nemesis::output::CLIP) {
        for (int channel = 0; channel < NEMESIS_NUM_OUTPUTS; channel++) {
            if (isAudioOutput(channel)) nemesis::output::shapeBlock(mode, fOutChannel[channel], n);
        }
    }

    // outputs into the transmit DMA half that plays next (zeroed by the
    // interrupt: unused channels stay silent)
    int32_t* out = nemesis::tdm::output();
    if (!out) return;
    for (int channel = 0; channel < NEMESIS_NUM_OUTPUTS && channel < nemesis::tdm::CHANNELS; channel++) {
        // per output gain and offset calibration of the module
        float scale, offset;
        nemesis::outputCorrection(channel, scale, offset);
        int32_t* dest = out + channel;
        uint32_t clipped = 0;  // samples beyond the DAC range, shown as CLIP
        for (int i = 0; i < n; i++) {
            float sample = fOutChannel[channel][i] * scale + offset;
            int32_t val;
            if (sample >= 1.0f) {
                val = INT32_MAX;
                clipped++;
            } else if (sample <= -1.0f) {
                val = INT32_MIN;
                clipped++;
            } else {
                val = (int32_t)(sample * MULT_32);
            }
            dest[i * nemesis::tdm::CHANNELS] = val;
        }
        countClip(channel, clipped);
    }
    nemesis::tdm::outputDone();
}

int NemesisAudio::numAudioInputs() const { return FAUST_INPUTS; }

int NemesisAudio::numAudioOutputs() const { return NEMESIS_NUM_OUTPUTS; }

void NemesisAudio::setParamValue(const std::string& path, float value)
{
    if (fUI) fUI->setParamValue(path, value);
}

float NemesisAudio::getParamValue(const std::string& path)
{
    return fUI ? fUI->getParamValue(path) : 0.0f;
}

int NemesisAudio::getParamsCount()
{
    return fUI ? fUI->getParamsCount() : 0;
}

// voices: the MIDI note of every voice (-1 free, -2 releasing, -3 legato)
FLASHMEM void NemesisAudio::printStatus(Print& out)
{
    out.printf("%s: %lu resets after inf/NaN output\n", name(), (unsigned long)fResets);
#ifdef MIDICTRL
#endif
    nemesis::printMidiStatus(out);
    out.print("clipped samples per output:");
    for (int i = 0; i < NEMESIS_NUM_OUTPUTS; i++) out.printf(" %lu", (unsigned long)clipCount(i));
    out.println();
#ifdef NVOICES
    if (!fVoices) return;
    mydsp_poly* poly = static_cast<mydsp_poly*>(fVoices);
    out.printf("%s: %d voices:", name(), (int)poly->fVoiceTable.size());
    for (dsp_voice* v : poly->fVoiceTable) out.printf(" %d", v->fCurNote);
    out.println();
#endif
}

std::string NemesisAudio::getParamAddress(int index)
{
    return fUI ? fUI->getParamAddress(index) : std::string();
}

/********************END ARCHITECTURE SECTION (part 2/2)****************/
