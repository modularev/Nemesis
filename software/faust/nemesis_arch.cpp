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

// 24 bit samples travel as two 16 bit TDM slots (MSB, LSB) per channel
#define MULT_32 2147483648.0f
#define DIV_32 4.656612873077392578125e-10f

#if defined(MIDICTRL) && defined(NEMESIS_FAUST_GLOBALS)
std::list<GUI*> GUI::fGuiList;
ztimedmap GUI::gTimedZoneMap;
#endif

// DSP memory: large buffers (delay lines, tables) go to the optional PSRAM,
// extmem_malloc() falls back to RAM when no PSRAM is fitted
#ifndef NEMESIS_EXTMEM_THRESHOLD
#define NEMESIS_EXTMEM_THRESHOLD 1024
#endif

struct NemesisAudioMemory : public dsp_memory_manager {
    void* allocate(size_t size) override
    {
        return (size >= NEMESIS_EXTMEM_THRESHOLD) ? extmem_malloc(size) : malloc(size);
    }
    void destroy(void* ptr) override
    {
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

FLASHMEM NemesisAudio::NemesisAudio() : nemesis::Program(2 * FAUST_INPUTS, new audio_block_t*[2 * FAUST_INPUTS]),
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

    fUI = new MapUI();
    fDSP->buildUserInterface(fUI);

    NemesisAudioBindUI bindUI(this);
    fDSP->buildUserInterface(&bindUI);

    fInChannel = new float*[FAUST_INPUTS];
    for (int i = 0; i < FAUST_INPUTS; i++) {
        fInChannel[i] = new float[AUDIO_BLOCK_SAMPLES];
    }
    fOutChannel = new float*[NEMESIS_NUM_OUTPUTS];
    for (int i = 0; i < NEMESIS_NUM_OUTPUTS; i++) {
        fOutChannel[i] = new float[AUDIO_BLOCK_SAMPLES];
    }

#ifdef MIDICTRL
    fMIDIHandler = new nemesis_midi();
    fMIDIInterface = new MidiUI(fMIDIHandler);
    // a polyphonic DSP registers itself for MIDI notes here (FAUST's poly-dsp.h)
    fDSP->buildUserInterface(fMIDIInterface);
    fMIDIInterface->run();
#endif
    return true;
}

FLASHMEM NemesisAudio::~NemesisAudio()
{
    if (!fDSP) return;
#ifdef MIDICTRL
    delete fMIDIInterface;
    delete fMIDIHandler;
#endif
    for (int i = 0; i < FAUST_INPUTS; i++) {
        delete[] fInChannel[i];
    }
    delete[] fInChannel;
    for (int i = 0; i < NEMESIS_NUM_OUTPUTS; i++) {
        delete[] fOutChannel[i];
    }
    delete[] fOutChannel;
    delete fUI;
#ifdef NVOICES
    delete fDSP;
#else
    mydsp::destroy(fDSP);
#endif
    mydsp::classDestroy();
}

void NemesisAudio::update(void)
{
    if (!fDSP) return;  // not started, only happens if patched without selectProgram()
#ifdef MIDICTRL
    // Process the MIDI messages received by the Teensy
    fMIDIHandler->processMidi();
    // MidiUI queues [midi:clock] toggles with a timestamp for a timed DSP,
    // which this architecture doesn't run: apply them now, else the queue
    // fills up and the clock never reaches the program
    for (auto& timed : GUI::gTimedZoneMap) {
        DatedControl dated(0, 0);
        while (ringbuffer_read_space(timed.second) >= sizeof(DatedControl)) {
            ringbuffer_read(timed.second, (char*)&dated, sizeof(DatedControl));
            *timed.first = dated.fValue;
        }
    }
    // Synchronize all GUI controllers
    GUI::updateAllGuis();
#endif

    for (int channel = 0; channel < FAUST_INPUTS; channel++) {
        // input polarity of the module
        float scale = DIV_32 * nemesis::inputScale(channel);
        audio_block_t* msb = receiveReadOnly(2 * channel);
        audio_block_t* lsb = receiveReadOnly(2 * channel + 1);
        if (msb) {
            for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
                uint16_t low = lsb ? (uint16_t)lsb->data[i] : 0;
                int32_t val = (int32_t)(((uint32_t)(uint16_t)msb->data[i] << 16) | low);
                fInChannel[channel][i] = val * scale;
            }
        } else {
            memset(fInChannel[channel], 0, AUDIO_BLOCK_SAMPLES * sizeof(float));
        }
        if (msb) release(msb);
        if (lsb) release(lsb);
    }

    fDSP->compute(AUDIO_BLOCK_SAMPLES, fInChannel, fOutChannel);

    // safety net: an unstable filter can leave inf/NaN in the DSP state, which
    // would silence the program for good, so clear the state and drop the block
    for (int channel = 0; channel < NEMESIS_NUM_OUTPUTS; channel++) {
        if (!std::isfinite(fOutChannel[channel][AUDIO_BLOCK_SAMPLES - 1])) {
            fDSP->instanceClear();
            for (int c = 0; c < NEMESIS_NUM_OUTPUTS; c++) {
                memset(fOutChannel[c], 0, AUDIO_BLOCK_SAMPLES * sizeof(float));
            }
            fResets++;
            break;
        }
    }

    for (int channel = 0; channel < NEMESIS_NUM_OUTPUTS; channel++) {
        // per output gain and offset calibration of the module
        float scale, offset;
        nemesis::outputCorrection(channel, scale, offset);
        audio_block_t* msb = allocate();
        audio_block_t* lsb = allocate();
        if (msb && lsb) {
            for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
                float sample = fOutChannel[channel][i] * scale + offset;
                int32_t val = (sample >= 1.0f) ? INT32_MAX : (sample <= -1.0f) ? INT32_MIN : (int32_t)(sample * MULT_32);
                msb->data[i] = val >> 16;
                lsb->data[i] = val & 0xFFFF;
            }
            transmit(msb, 2 * channel);
            transmit(lsb, 2 * channel + 1);
        }
        if (msb) release(msb);
        if (lsb) release(lsb);
    }
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
    out.printf("MIDI: %lu bytes on the MIDI jack (last status 0x%02X), %lu USB messages\n",
               (unsigned long)nemesis_midi::fSerialBytes, nemesis_midi::fLastStatus,
               (unsigned long)nemesis_midi::fUSBMessages);
#endif
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
