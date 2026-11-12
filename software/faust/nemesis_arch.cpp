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
//   MIDICTRL                 [midi:on] in 'declare options' (the library
//                            handles MIDI for every program, see update())
//   NVOICES <n>              [nvoices:n] in 'declare options'
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

// MIDI: the library maps FAUST's [midi:..] metadata (nemesis_program.h);
// polyphonic programs: voices without FAUST's poly-dsp.h/MidiUI, which kept
// a program from being freed (unique name: FAUST searches its install dirs
// before -A)
#ifdef NVOICES
#include "faust/dsp/nemesis-poly.h"
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

// DSP memory: small allocations from the heap; delay lines up to
// NEMESIS_RAM_BUFFER_MAX from the library's RAM arena (fast, read on every
// sample: PSRAM reads cost a lot of CPU, e.g. mooSpace), the rest from the
// optional PSRAM (extmem_malloc() falls back to RAM without PSRAM).
// The arena is emptied whenever a program stops.
#ifndef NEMESIS_EXTMEM_THRESHOLD
#define NEMESIS_EXTMEM_THRESHOLD 1024
#endif
#ifndef NEMESIS_RAM_BUFFER_MAX
#define NEMESIS_RAM_BUFFER_MAX 32768
#endif

// the large delay lines (>= 256 kB) in allocation order: a frozen loop the
// library saves on the SD card (nemesis_loop.h)
#define NEMESIS_LARGE_BUFFER 262144
#define NEMESIS_MAX_LARGE 4
static float* gLargeBuffer[NEMESIS_MAX_LARGE];
static uint32_t gLargeLength[NEMESIS_MAX_LARGE];
static int gLargeCount = 0;

struct NemesisAudioMemory : public dsp_memory_manager {
    void* allocate(size_t size) override
    {
        if (size < NEMESIS_EXTMEM_THRESHOLD) return malloc(size);
        if (size >= NEMESIS_LARGE_BUFFER) {
            void* p = extmem_malloc(size);
            if (p && gLargeCount < NEMESIS_MAX_LARGE) {
                gLargeBuffer[gLargeCount] = (float*)p;
                gLargeLength[gLargeCount++] = size / sizeof(float);
            }
            NEMESIS_TRACE("  alloc %u psram (large)\n", (unsigned)size);
            return p;
        }
        if (size <= NEMESIS_RAM_BUFFER_MAX) {
            void* p = nemesis::arenaAllocate(size);
            if (p) { NEMESIS_TRACE("  alloc %u arena\n", (unsigned)size); return p; }
        }
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
    fInChannel(nullptr), fOutChannel(nullptr), fDSP(nullptr), fVoices(nullptr), fResets(0)
{
}

// creates the DSP (memory in PSRAM where possible) and binds the controls
FLASHMEM bool NemesisAudio::start()
{
    if (fDSP) return true;

    mydsp::fManager = &gNemesisAudioMemory;
    gLargeCount = 0;
    fSamples = 0;  // the delay lines start writing at 0 (instanceInit)
#ifdef NVOICES
    // the voices, each made by the memory manager (freed by stop())
    dsp* voices[NVOICES];
    for (int i = 0; i < NVOICES; i++) {
        voices[i] = mydsp::create();
    }
    dsp* effect = nullptr;
    void (*destroyEffect)(dsp*) = nullptr;
#ifdef NEMESIS_EFFECT
    // the effect runs once after the voice mix
    NemesisAudio_effect::fManager = &gNemesisAudioMemory;
    effect = NemesisAudio_effect::create();
    destroyEffect = NemesisAudio_effect::destroy;
#endif
    nemesis_poly* poly = new nemesis_poly(voices, NVOICES, mydsp::destroy, effect, destroyEffect, AUDIO_BLOCK_SAMPLES);
    fVoices = poly;
    fDSP = poly;
#else
    fDSP = mydsp::create();
#endif
    // the program's metadata first: 'declare sample_rate' sets the hardware
    // before the DSP is initialized for that rate
    NemesisAudioMeta meta(this);
    fDSP->metadata(&meta);
    const int rate = nemesis::applySampleRate(*this);

    // with -mem, init() is empty: classInit() plus instanceInit()
    NEMESIS_TRACE("  classInit %d\n", rate);
    mydsp::classInit(rate);
#ifdef NEMESIS_EFFECT
    NemesisAudio_effect::classInit(rate);
#endif
    fDSP->instanceInit(rate);
#ifdef NVOICES
    // [nvoices:N] are the voices at 48 kHz: fewer at a higher rate (each costs
    // about twice as much there)
    poly->setVoiceLimit(NVOICES * 48000 / rate);
#endif

    NEMESIS_TRACE("  bind\n");
    NemesisAudioBindUI bindUI(this);
    fDSP->buildUserInterface(&bindUI);
    bindOutputSetting();  // "output": clip/soft/tape/fold of the audio outputs

    fInChannel = new float*[FAUST_INPUTS];
    for (int i = 0; i < FAUST_INPUTS; i++) {
        fInChannel[i] = new float[AUDIO_BLOCK_SAMPLES];
    }
    fOutChannel = new float*[NEMESIS_NUM_OUTPUTS];
    for (int i = 0; i < NEMESIS_NUM_OUTPUTS; i++) {
        fOutChannel[i] = new float[AUDIO_BLOCK_SAMPLES];
    }

    NEMESIS_TRACE("  restore\n");
    restoreBindings();  // settings and CV routing from before a stop()
    fReady = true;
    return true;
}

int NemesisAudio::largeBuffers(float** buffers, uint32_t* lengths, int max) const
{
    if (!fDSP) return 0;
    int n = 0;
    for (; n < gLargeCount && n < max; n++) {
        buffers[n] = gLargeBuffer[n];
        lengths[n] = gLargeLength[n];
    }
    return n;
}

FLASHMEM NemesisAudio::~NemesisAudio()
{
    stop();
}

// frees the DSP and its buffers (all programs); start() builds them again
FLASHMEM void NemesisAudio::stop()
{
    if (!fDSP) return;
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
#ifdef NVOICES
    delete fDSP;  // the voices and the effect with their destroy()
    fVoices = nullptr;
#ifdef NEMESIS_EFFECT
    NemesisAudio_effect::classDestroy();
#endif
#else
    mydsp::destroy(fDSP);
#endif
    NEMESIS_TRACE("  classDestroy\n");
    fDSP = nullptr;
    gLargeCount = 0;
    mydsp::classDestroy();
    nemesis::arenaReset();  // this program's delay lines
}

void NemesisAudio::update(void)
{
    if (!fReady) return;  // not started, or start() is still allocating
    // samples in this block (set at runtime, at most the buffers' 128)
    const int n = nemesis::tdm::blockSize();
    // transport position for this block ([transport:..] parameters)
    beginBlock();
    // MIDI received in the background: [mono:..] and [midi:..] parameters
    // (the library), then the voices; programs without MIDI only empty the
    // queue, so old messages don't reach the next program
    nemesis::MidiMessage message;
    while (nemesis::midiRead(message)) {
        handleMidi(message);
#ifdef NVOICES
        int listen = nemesis::midiChannel();
        if (message.status < 0xF0 && (listen == 0 || (message.status & 0x0F) == listen - 1)) {
            // FAUST's midi API (as a MidiUI would call it): channel 0..15
            nemesis_poly* poly = static_cast<nemesis_poly*>(fVoices);
            int channel = message.status & 0x0F;
            switch (message.status & 0xF0) {
                case 0x90: poly->keyOn(channel, message.data1, message.data2); break;
                case 0x80: poly->keyOff(channel, message.data1, message.data2); break;
                case 0xB0: poly->ctrlChange(channel, message.data1, message.data2); break;  // CC 120/123
            }
        }
#endif
    }

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

    if (nemesis::debugMute)
        for (int c = 0; c < NEMESIS_NUM_OUTPUTS; c++) memset(fOutChannel[c], 0, n * sizeof(float));
    else
        fDSP->compute(n, fInChannel, fOutChannel);
    if (nemesis::debugSilent)
        for (int c = 0; c < NEMESIS_NUM_OUTPUTS; c++) memset(fOutChannel[c], 0, n * sizeof(float));
    fSamples += n;  // instanceClear() below keeps the delay lines' write position

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

    // fades around program switches and audio restarts (nemesis_program.h)
    float g = nemesis::outputGain;
    const float target = nemesis::outputTarget;
    if (g != target || g != 1.0f) {
        const float rate = nemesis::sampleRate();
        const float step = target < g ? 1.0f / (0.005f * rate) : 1.0f / (0.010f * rate);
        float gain = g;
        for (int i = 0; i < n; i++) {
            gain = target < gain ? fmaxf(target, gain - step) : fminf(target, gain + step);
            for (int c = 0; c < NEMESIS_NUM_OUTPUTS; c++) fOutChannel[c][i] *= gain;
        }
        nemesis::outputGain = gain;
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

// parameters by path ("/strike/v1_pitch") or label, for the serial GET:
// found by walking the UI when asked (FAUST's MapUI kept maps of every path
// and name: ~60 kB of heap for strike, which ran out after program switches)
struct NemesisFindUI : public UI {
    const char* fWanted;  // a path or label, or nullptr: the fIndex-th parameter
    int fIndex;
    int fCount = 0;
    FAUSTFLOAT* fZone = nullptr;
    std::string fPath;
    char fPrefix[256];
    int fLength = 0;
    int fLevels[16];
    int fDepth = 0;

    NemesisFindUI(const char* wanted, int index) : fWanted(wanted), fIndex(index) { fPrefix[0] = 0; }

    // FAUST's PathBuilder: '/' in a label and the characters below become '_'
    static char clean(char c) { return strchr("/ #*,?[]{}()", c) ? '_' : c; }
    void append(const char* label)
    {
        for (; *label && fLength < (int)sizeof(fPrefix) - 2; label++) fPrefix[fLength++] = clean(*label);
        fPrefix[fLength] = 0;
    }
    void push(const char* label)
    {
        if (fDepth < 16) fLevels[fDepth] = fLength;
        fDepth++;
        append("");
        if (fLength < (int)sizeof(fPrefix) - 2) fPrefix[fLength++] = '/';
        append(label);
    }
    void add(const char* label, FAUSTFLOAT* zone)
    {
        if (fZone) return;
        int base = fLength;
        if (fLength < (int)sizeof(fPrefix) - 2) fPrefix[fLength++] = '/';
        append(label);
        bool match = fWanted ? (strcmp(fPrefix, fWanted) == 0 || strcmp(label, fWanted) == 0) : fCount == fIndex;
        if (match) {
            fZone = zone;
            fPath = fPrefix;
        }
        fCount++;
        fLength = base;
        fPrefix[fLength] = 0;
    }

    void openTabBox(const char* label) override { push(label); }
    void openHorizontalBox(const char* label) override { push(label); }
    void openVerticalBox(const char* label) override { push(label); }
    void closeBox() override
    {
        fDepth--;
        if (fDepth >= 0 && fDepth < 16) fLength = fLevels[fDepth];
        fPrefix[fLength] = 0;
    }
    void addButton(const char* label, FAUSTFLOAT* zone) override { add(label, zone); }
    void addCheckButton(const char* label, FAUSTFLOAT* zone) override { add(label, zone); }
    void addVerticalSlider(const char* label, FAUSTFLOAT* zone, FAUSTFLOAT, FAUSTFLOAT, FAUSTFLOAT, FAUSTFLOAT) override { add(label, zone); }
    void addHorizontalSlider(const char* label, FAUSTFLOAT* zone, FAUSTFLOAT, FAUSTFLOAT, FAUSTFLOAT, FAUSTFLOAT) override { add(label, zone); }
    void addNumEntry(const char* label, FAUSTFLOAT* zone, FAUSTFLOAT, FAUSTFLOAT, FAUSTFLOAT, FAUSTFLOAT) override { add(label, zone); }
    void addHorizontalBargraph(const char* label, FAUSTFLOAT* zone, FAUSTFLOAT, FAUSTFLOAT) override { add(label, zone); }
    void addVerticalBargraph(const char* label, FAUSTFLOAT* zone, FAUSTFLOAT, FAUSTFLOAT) override { add(label, zone); }
    void addSoundfile(const char*, const char*, Soundfile**) override {}
};

void NemesisAudio::setParamValue(const std::string& path, float value)
{
    if (!fDSP) return;
    NemesisFindUI find(path.c_str(), -1);
    fDSP->buildUserInterface(&find);
    if (find.fZone) *find.fZone = value;
}

float NemesisAudio::getParamValue(const std::string& path)
{
    if (!fDSP) return 0.0f;
    NemesisFindUI find(path.c_str(), -1);
    fDSP->buildUserInterface(&find);
    return find.fZone ? *find.fZone : 0.0f;
}

int NemesisAudio::getParamsCount()
{
    if (!fDSP) return 0;
    NemesisFindUI find(nullptr, -1);
    fDSP->buildUserInterface(&find);
    return find.fCount;
}

// voices: the MIDI note of every voice (-1 free, -2 releasing)
FLASHMEM void NemesisAudio::printStatus(Print& out)
{
    out.printf("%s: %lu resets after inf/NaN output\n", name(), (unsigned long)fResets);
    nemesis::printMidiStatus(out);
    out.print("clipped samples per output:");
    for (int i = 0; i < NEMESIS_NUM_OUTPUTS; i++) out.printf(" %lu", (unsigned long)clipCount(i));
    out.println();
#ifdef NVOICES
    if (!fVoices) return;
    nemesis_poly* poly = static_cast<nemesis_poly*>(fVoices);
    out.printf("%s: %d voices (%d in use):", name(), poly->voices(), poly->voiceLimit());
    for (int i = 0; i < poly->voices(); i++) out.printf(" %d", poly->voiceNote(i));
    out.println();
#endif
}

std::string NemesisAudio::getParamAddress(int index)
{
    if (!fDSP) return std::string();
    NemesisFindUI find(nullptr, index);
    fDSP->buildUserInterface(&find);
    return find.fPath;
}

/********************END ARCHITECTURE SECTION (part 2/2)****************/
