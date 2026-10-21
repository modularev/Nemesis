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

/***************************END USER SECTION ***************************/

/*******************BEGIN ARCHITECTURE SECTION (part 2/2)***************/

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
        fProgram->bindControl(label, zone, 0, 1, true);
    }
    void addCheckButton(const char* label, FAUSTFLOAT* zone) override
    {
        fProgram->bindControl(label, zone, 0, 1, true);
    }
    void addVerticalSlider(const char* label, FAUSTFLOAT* zone, FAUSTFLOAT init, FAUSTFLOAT min, FAUSTFLOAT max, FAUSTFLOAT step) override
    {
        fProgram->bindControl(label, zone, min, max, false);
    }
    void addHorizontalSlider(const char* label, FAUSTFLOAT* zone, FAUSTFLOAT init, FAUSTFLOAT min, FAUSTFLOAT max, FAUSTFLOAT step) override
    {
        fProgram->bindControl(label, zone, min, max, false);
    }
    void addNumEntry(const char* label, FAUSTFLOAT* zone, FAUSTFLOAT init, FAUSTFLOAT min, FAUSTFLOAT max, FAUSTFLOAT step) override
    {
        fProgram->bindControl(label, zone, min, max, false);
    }

    // outputs are never bound, only drop their metadata
    void addHorizontalBargraph(const char* label, FAUSTFLOAT* zone, FAUSTFLOAT min, FAUSTFLOAT max) override
    {
        fProgram->bindControl(label, nullptr, min, max, false);
    }
    void addVerticalBargraph(const char* label, FAUSTFLOAT* zone, FAUSTFLOAT min, FAUSTFLOAT max) override
    {
        fProgram->bindControl(label, nullptr, min, max, false);
    }
    void addSoundfile(const char* label, const char* filename, Soundfile** sf_zone) override {}

    void declare(FAUSTFLOAT* zone, const char* key, const char* val) override
    {
        if (zone) fProgram->declareControl(key, val);
    }
};

FLASHMEM NemesisAudio::NemesisAudio() : nemesis::Program(2 * FAUST_INPUTS, new audio_block_t*[2 * FAUST_INPUTS])
{
    mydsp::fManager = &gNemesisAudioMemory;
    mydsp::classInit(AUDIO_SAMPLE_RATE_EXACT);
#ifdef NVOICES
    // voices are cloned with mydsp::create(), so they use the memory manager too
    fDSP = new mydsp_poly(mydsp::create(), NVOICES, true, true);
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
    fOutChannel = new float*[FAUST_OUTPUTS];
    for (int i = 0; i < FAUST_OUTPUTS; i++) {
        fOutChannel[i] = new float[AUDIO_BLOCK_SAMPLES];
    }

    fMIDIHandler = nullptr;
    fMIDIInterface = nullptr;
#ifdef MIDICTRL
    fMIDIHandler = new nemesis_midi();
    fMIDIInterface = new MidiUI(fMIDIHandler);
    fDSP->buildUserInterface(fMIDIInterface);
#ifdef NVOICES
    fMIDIHandler->addMidiIn(static_cast<mydsp_poly*>(fDSP));
#endif
    fMIDIInterface->run();
#endif
}

FLASHMEM NemesisAudio::~NemesisAudio()
{
#ifdef MIDICTRL
    delete fMIDIInterface;
    delete fMIDIHandler;
#endif
    for (int i = 0; i < FAUST_INPUTS; i++) {
        delete[] fInChannel[i];
    }
    delete[] fInChannel;
    for (int i = 0; i < FAUST_OUTPUTS; i++) {
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
#ifdef MIDICTRL
    // Process the MIDI messages received by the Teensy
    fMIDIHandler->processMidi();
    // Synchronize all GUI controllers
    GUI::updateAllGuis();
#endif

    for (int channel = 0; channel < FAUST_INPUTS; channel++) {
        audio_block_t* msb = receiveReadOnly(2 * channel);
        audio_block_t* lsb = receiveReadOnly(2 * channel + 1);
        if (msb) {
            for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
                uint16_t low = lsb ? (uint16_t)lsb->data[i] : 0;
                int32_t val = (int32_t)(((uint32_t)(uint16_t)msb->data[i] << 16) | low);
                fInChannel[channel][i] = val * DIV_32;
            }
        } else {
            memset(fInChannel[channel], 0, AUDIO_BLOCK_SAMPLES * sizeof(float));
        }
        if (msb) release(msb);
        if (lsb) release(lsb);
    }

    fDSP->compute(AUDIO_BLOCK_SAMPLES, fInChannel, fOutChannel);

    for (int channel = 0; channel < FAUST_OUTPUTS; channel++) {
        audio_block_t* msb = allocate();
        audio_block_t* lsb = allocate();
        if (msb && lsb) {
            for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
                float sample = fOutChannel[channel][i];
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

int NemesisAudio::numAudioOutputs() const { return FAUST_OUTPUTS; }

void NemesisAudio::setParamValue(const std::string& path, float value)
{
    fUI->setParamValue(path, value);
}

float NemesisAudio::getParamValue(const std::string& path)
{
    return fUI->getParamValue(path);
}

int NemesisAudio::getParamsCount()
{
    return fUI->getParamsCount();
}

std::string NemesisAudio::getParamAddress(int index)
{
    return fUI->getParamAddress(index);
}

/********************END ARCHITECTURE SECTION (part 2/2)****************/
