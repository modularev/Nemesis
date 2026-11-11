/************************************************************************
 FAUST polyphony for microcontrollers (NEMESIS)
 Copyright (C) 2026 Arev Imer
 ---------------------------------------------------------------------
 This file is free software; you can redistribute it and/or modify it
 under the terms of the GNU Lesser General Public License as published by
 the Free Software Foundation; either version 2.1 of the License, or (at
 your option) any later version.
 ************************************************************************/

// The voices of a standard FAUST polyphonic program ('declare options
// "[nvoices:N]"', parameters freq/key, gain/vel/velocity, gate; an optional
// 'effect' after the voice mix), like FAUST's poly-dsp.h but for small
// systems:
//
// - the voices come from the program's create()/destroy() (-mem: the
//   architecture's memory manager), so a program can be freed completely;
// - no std::map, std::string, MapUI or RTTI: a voice keeps pointers to its
//   zones only (FAUST's dsp_voice is a MapUI: ~35 kB of heap per voice of
//   juno on a Teensy 4.1);
// - the voices share their other parameters: the UI shows the first voice's
//   (and the effect's), copied to the others before every block;
// - silent voices are skipped: a voice is computed from its note on until
//   its output stayed below VOICE_STOP_LEVEL (-66 dB, as FAUST) for 10 ms
//   after the note off;
// - a new note takes the free voice unused longest (notes move through the
//   voices, as on the Juno), else the oldest releasing one, else the oldest
//   playing one. A voice that still sounds fades out in 3 ms with
//   its gate off, takes the new note and fades in over 3 ms: no jump in
//   pitch or level at full volume (stealing clicked with only a block of
//   gate off); the envelopes go on from their level, as a retrigger.
//
// - a parameter with [poly:voice] gets the voice's number (0, 1, ..; not
//   shared), e.g. for a fixed place in the stereo field per voice; in
//   FAUST's own poly-dsp.h it keeps its default.
//
// MIDI notes come from the architecture: keyOn(), keyOff(), allNotesOff().
// The program's MIDI parameters ([midi:ctrl 1] ..) are the architecture's
// job too (they are set on the first voice).

#ifndef __nemesis_poly__
#define __nemesis_poly__

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "faust/dsp/dsp.h"
#include "faust/gui/UI.h"

class nemesis_poly : public dsp {

    public:
        typedef void (*destroy_fn)(dsp*);

        static constexpr float VOICE_STOP_LEVEL = 0.0005f;  // -66 dB, as FAUST's poly-dsp.h
        static constexpr int MAX_VOICES = 16;
        static constexpr int MAX_CHANNELS = 16;
        static constexpr int MAX_SHARED = 256;  // parameters copied to all voices

    private:
        enum { FREE = -1 };

        struct Voice {
            dsp* fDSP = nullptr;
            FAUSTFLOAT* fFreq = nullptr;
            FAUSTFLOAT* fKey = nullptr;
            FAUSTFLOAT* fGain = nullptr;
            FAUSTFLOAT* fVel = nullptr;
            FAUSTFLOAT* fGate = nullptr;
            FAUSTFLOAT* fIndex = nullptr;   // [poly:voice]
            int fNote = FREE;         // the note it plays (also while releasing)
            bool fHeld = false;       // gate on
            int fNextNote = FREE;     // stolen: the note after the fade out
            int fNextVel = 0;
            int fFadeOut = 0;         // samples left of the fade out (stolen)
            int fFadeIn = 0;          // samples left of the fade in (after a steal)
            uint32_t fAge = 0;        // note on order
            int fQuiet = 0;           // samples below VOICE_STOP_LEVEL after the note off
        };

        // a voice's zones in the order of its UI: the same order in every voice
        struct ZoneList : public UI {
            FAUSTFLOAT** fZones;
            int fMax;
            int fCount = 0;
            Voice* fVoice;
            FAUSTFLOAT* fIndexZone = nullptr;   // declared [poly:voice]
            ZoneList(FAUSTFLOAT** zones, int max, Voice* voice) : fZones(zones), fMax(max), fVoice(voice) {}

            static bool is(const char* label, const char* name) { return strcmp(label, name) == 0; }
            void add(const char* label, FAUSTFLOAT* zone)
            {
                // FAUST's poly-dsp.h finds them by the end of the path
                if (is(label, "freq")) fVoice->fFreq = zone;
                else if (is(label, "key")) fVoice->fKey = zone;
                else if (is(label, "gain")) fVoice->fGain = zone;
                else if (is(label, "vel") || is(label, "velocity")) fVoice->fVel = zone;
                else if (is(label, "gate")) fVoice->fGate = zone;
                else if (zone == fIndexZone) fVoice->fIndex = zone;
                else if (fZones && fCount < fMax) fZones[fCount++] = zone;
                else fCount++;
            }
            void openTabBox(const char*) override {}
            void openHorizontalBox(const char*) override {}
            void openVerticalBox(const char*) override {}
            void closeBox() override {}
            void addButton(const char* l, FAUSTFLOAT* z) override { add(l, z); }
            void addCheckButton(const char* l, FAUSTFLOAT* z) override { add(l, z); }
            void addVerticalSlider(const char* l, FAUSTFLOAT* z, FAUSTFLOAT, FAUSTFLOAT, FAUSTFLOAT, FAUSTFLOAT) override { add(l, z); }
            void addHorizontalSlider(const char* l, FAUSTFLOAT* z, FAUSTFLOAT, FAUSTFLOAT, FAUSTFLOAT, FAUSTFLOAT) override { add(l, z); }
            void addNumEntry(const char* l, FAUSTFLOAT* z, FAUSTFLOAT, FAUSTFLOAT, FAUSTFLOAT, FAUSTFLOAT) override { add(l, z); }
            void addHorizontalBargraph(const char*, FAUSTFLOAT*, FAUSTFLOAT, FAUSTFLOAT) override {}
            void addVerticalBargraph(const char*, FAUSTFLOAT*, FAUSTFLOAT, FAUSTFLOAT) override {}
            void addSoundfile(const char*, const char*, Soundfile**) override {}
            void declare(FAUSTFLOAT* zone, const char* key, const char* value) override
            {
                if (zone && strcmp(key, "poly") == 0 && strcmp(value, "voice") == 0) fIndexZone = zone;
            }
        };

        // the first voice's UI without its note parameters (they belong to
        // the voice allocation): passed on with a null zone
        struct HideNotes : public UI {
            UI* fUI;
            const Voice* fVoice;
            HideNotes(UI* ui, const Voice* voice) : fUI(ui), fVoice(voice) {}
            FAUSTFLOAT* visible(FAUSTFLOAT* z) const
            {
                return (z == fVoice->fFreq || z == fVoice->fKey || z == fVoice->fGain || z == fVoice->fVel || z == fVoice->fGate ||
                        z == fVoice->fIndex) ? nullptr : z;
            }
            void openTabBox(const char* l) override { fUI->openTabBox(l); }
            void openHorizontalBox(const char* l) override { fUI->openHorizontalBox(l); }
            void openVerticalBox(const char* l) override { fUI->openVerticalBox(l); }
            void closeBox() override { fUI->closeBox(); }
            void addButton(const char* l, FAUSTFLOAT* z) override { fUI->addButton(l, visible(z)); }
            void addCheckButton(const char* l, FAUSTFLOAT* z) override { fUI->addCheckButton(l, visible(z)); }
            void addVerticalSlider(const char* l, FAUSTFLOAT* z, FAUSTFLOAT i, FAUSTFLOAT a, FAUSTFLOAT b, FAUSTFLOAT s) override { fUI->addVerticalSlider(l, visible(z), i, a, b, s); }
            void addHorizontalSlider(const char* l, FAUSTFLOAT* z, FAUSTFLOAT i, FAUSTFLOAT a, FAUSTFLOAT b, FAUSTFLOAT s) override { fUI->addHorizontalSlider(l, visible(z), i, a, b, s); }
            void addNumEntry(const char* l, FAUSTFLOAT* z, FAUSTFLOAT i, FAUSTFLOAT a, FAUSTFLOAT b, FAUSTFLOAT s) override { fUI->addNumEntry(l, visible(z), i, a, b, s); }
            void addHorizontalBargraph(const char* l, FAUSTFLOAT* z, FAUSTFLOAT a, FAUSTFLOAT b) override { fUI->addHorizontalBargraph(l, z, a, b); }
            void addVerticalBargraph(const char* l, FAUSTFLOAT* z, FAUSTFLOAT a, FAUSTFLOAT b) override { fUI->addVerticalBargraph(l, z, a, b); }
            void addSoundfile(const char* l, const char* f, Soundfile** s) override { fUI->addSoundfile(l, f, s); }
            void declare(FAUSTFLOAT* z, const char* k, const char* v) override { fUI->declare(z ? visible(z) : z, k, v); }
        };

        Voice fVoices[MAX_VOICES];
        int fCount;
        int fLimit;               // voices in use (setVoiceLimit)
        destroy_fn fDestroyVoice;
        dsp* fEffect;
        destroy_fn fDestroyEffect;
        FAUSTFLOAT** fShared;     // [voice * fSharedCount + i]
        int fSharedCount;
        int fBlock;               // largest block size
        int fVoiceOutputs;
        FAUSTFLOAT* fVoiceOut[MAX_CHANNELS];   // one voice's output
        FAUSTFLOAT* fMix[MAX_CHANNELS];        // the sum of the voices (the effect's input)
        uint32_t fClock = 0;
        int fFade;                // fade length in samples (3 ms)

        static float noteHz(int note) { return 440.0f * powf(2.0f, (note - 69) / 12.0f); }

        void start(Voice& v, int note, int velocity)
        {
            v.fNote = note;
            v.fHeld = true;
            v.fAge = ++fClock;
            v.fQuiet = 0;
            v.fFadeOut = 0;
            if (v.fFreq) *v.fFreq = noteHz(note);
            if (v.fKey) *v.fKey = note;
            if (v.fGain) *v.fGain = velocity / 127.0f;
            if (v.fVel) *v.fVel = velocity;
            if (v.fGate) *v.fGate = 1;
        }

        Voice* allocate()
        {
            Voice* free = nullptr;
            Voice* releasing = nullptr;
            Voice* playing = nullptr;
            for (int i = 0; i < fLimit; i++) {
                Voice& v = fVoices[i];
                if (v.fNote == FREE && v.fNextNote == FREE) {
                    if (!free || v.fAge < free->fAge) free = &v;  // unused longest
                    continue;
                }
                if (v.fFadeOut > 0) continue;  // already being stolen
                if (!v.fHeld) { if (!releasing || v.fAge < releasing->fAge) releasing = &v; }
                else if (!playing || v.fAge < playing->fAge) playing = &v;
            }
            return free ? free : releasing ? releasing : playing;
        }

    public:
        // voices: count instances made by the program's create(), destroyed
        // with destroy_voice; effect (or null): runs once on the voice mix
        nemesis_poly(dsp** voices, int count, destroy_fn destroy_voice, dsp* effect, destroy_fn destroy_effect, int block)
            : fCount(count < MAX_VOICES ? count : MAX_VOICES), fLimit(fCount), fDestroyVoice(destroy_voice),
              fEffect(effect), fDestroyEffect(destroy_effect), fBlock(block)
        {
            for (int i = 0; i < fCount; i++) fVoices[i].fDSP = voices[i];
            // the shared parameters, in UI order (the same in every voice)
            ZoneList counter(nullptr, 0, &fVoices[0]);
            fVoices[0].fDSP->buildUserInterface(&counter);
            fSharedCount = counter.fCount < MAX_SHARED ? counter.fCount : MAX_SHARED;
            fShared = new FAUSTFLOAT*[fCount * fSharedCount + 1];
            for (int i = 0; i < fCount; i++) {
                ZoneList list(fShared + i * fSharedCount, fSharedCount, &fVoices[i]);
                fVoices[i].fDSP->buildUserInterface(&list);
            }
            fFade = 144;  // 3 ms at 48 kHz, set again by instanceInit()
            setIndices();
            fVoiceOutputs = fVoices[0].fDSP->getNumOutputs();
            if (fVoiceOutputs > MAX_CHANNELS) fVoiceOutputs = MAX_CHANNELS;
            for (int c = 0; c < fVoiceOutputs; c++) {
                fVoiceOut[c] = new FAUSTFLOAT[fBlock];
                fMix[c] = new FAUSTFLOAT[fBlock];
            }
        }

        virtual ~nemesis_poly()
        {
            for (int i = 0; i < fCount; i++) fDestroyVoice(fVoices[i].fDSP);
            if (fEffect) fDestroyEffect(fEffect);
            delete[] fShared;
            for (int c = 0; c < fVoiceOutputs; c++) {
                delete[] fVoiceOut[c];
                delete[] fMix[c];
            }
        }

        // notes

        void keyOn(int note, int velocity)
        {
            if (velocity == 0) { keyOff(note); return; }
            Voice* v = allocate();
            if (!v) return;
            if (v->fNote != FREE) {
                // still sounding (playing or releasing): fade out with the
                // gate off, then the new note fades in
                if (v->fGate) *v->fGate = 0;
                v->fHeld = false;
                v->fNextNote = note;
                v->fNextVel = velocity;
                // from its level: a voice still fading in starts lower
                v->fFadeOut = fFade - v->fFadeIn;
                v->fFadeIn = 0;
                v->fAge = ++fClock;
            } else {
                v->fFadeIn = 0;
                start(*v, note, velocity);
            }
        }

        void keyOff(int note)
        {
            for (int i = 0; i < fCount; i++) {
                Voice& v = fVoices[i];
                if (v.fNextNote == note) v.fNextNote = FREE;  // released before it started (the fade out goes on)
                if (v.fHeld && v.fNote == note) {
                    v.fHeld = false;
                    v.fQuiet = 0;
                    if (v.fGate) *v.fGate = 0;
                }
            }
        }

        void allNotesOff()
        {
            for (int i = 0; i < fCount; i++) {
                Voice& v = fVoices[i];
                v.fNextNote = FREE;
                if (v.fHeld) {
                    v.fHeld = false;
                    v.fQuiet = 0;
                    if (v.fGate) *v.fGate = 0;
                }
            }
        }

        int voices() const { return fCount; }
        // only the first n voices take notes (e.g. fewer at a higher sample
        // rate); the others finish their notes
        void setVoiceLimit(int n) { fLimit = n < 1 ? 1 : n > fCount ? fCount : n; }
        int voiceLimit() const { return fLimit; }
        // the note of a voice, -1 free, -2 releasing
        int voiceNote(int i) const
        {
            const Voice& v = fVoices[i];
            return v.fNote == FREE ? -1 : v.fHeld ? v.fNote : -2;
        }

        // dsp

        int getNumInputs() override { return fVoices[0].fDSP->getNumInputs(); }
        int getNumOutputs() override { return fEffect ? fEffect->getNumOutputs() : fVoiceOutputs; }
        void buildUserInterface(UI* ui) override
        {
            HideNotes hide(ui, &fVoices[0]);
            fVoices[0].fDSP->buildUserInterface(&hide);
            if (fEffect) fEffect->buildUserInterface(ui);
        }
        int getSampleRate() override { return fVoices[0].fDSP->getSampleRate(); }
        void init(int sample_rate) override { instanceInit(sample_rate); }
        // the voices' numbers (instanceInit and instanceResetUserInterface set the defaults)
        void setIndices()
        {
            for (int i = 0; i < fCount; i++)
                if (fVoices[i].fIndex) *fVoices[i].fIndex = i;
        }

        void instanceInit(int sample_rate) override
        {
            fFade = sample_rate * 3 / 1000 > 0 ? sample_rate * 3 / 1000 : 1;
            for (int i = 0; i < fCount; i++) fVoices[i].fDSP->instanceInit(sample_rate);
            if (fEffect) fEffect->instanceInit(sample_rate);
            setIndices();
        }
        void instanceConstants(int sample_rate) override
        {
            for (int i = 0; i < fCount; i++) fVoices[i].fDSP->instanceConstants(sample_rate);
            if (fEffect) fEffect->instanceConstants(sample_rate);
        }
        void instanceResetUserInterface() override
        {
            for (int i = 0; i < fCount; i++) fVoices[i].fDSP->instanceResetUserInterface();
            if (fEffect) fEffect->instanceResetUserInterface();
            setIndices();
        }
        void instanceClear() override
        {
            for (int i = 0; i < fCount; i++) fVoices[i].fDSP->instanceClear();
            if (fEffect) fEffect->instanceClear();
        }
        dsp* clone() override { return nullptr; }
        void metadata(Meta* m) override { fVoices[0].fDSP->metadata(m); }

        void compute(int count, FAUSTFLOAT** inputs, FAUSTFLOAT** outputs) override
        {
            if (count > fBlock) count = fBlock;
            // the first voice's parameters (set by the UI) to the others
            for (int i = 1; i < fCount; i++) {
                FAUSTFLOAT** to = fShared + i * fSharedCount;
                for (int k = 0; k < fSharedCount; k++) *to[k] = *fShared[k];
            }
            FAUSTFLOAT** mix = fEffect ? fMix : outputs;
            for (int c = 0; c < fVoiceOutputs; c++) memset(mix[c], 0, count * sizeof(FAUSTFLOAT));
            for (int i = 0; i < fCount; i++) {
                Voice& v = fVoices[i];
                if (v.fNote == FREE && v.fNextNote == FREE) continue;  // silent: skipped
                v.fDSP->compute(count, inputs, fVoiceOut);
                // fades: out before a stolen voice's new note, in after it
                bool fading = v.fFadeOut > 0 || v.fFadeIn > 0;
                int fadeOut = v.fFadeOut, fadeIn = v.fFadeIn;
                float peak = 0.0f;
                for (int c = 0; c < fVoiceOutputs; c++) {
                    const FAUSTFLOAT* in = fVoiceOut[c];
                    FAUSTFLOAT* out = mix[c];
                    if (fading) {
                        int fo = fadeOut, fi = fadeIn;
                        const float step = 1.0f / fFade;
                        for (int s = 0; s < count; s++) {
                            float g = 1.0f;
                            if (fadeOut > 0) { g = fo > 0 ? fo * step : 0.0f; if (fo > 0) fo--; }
                            else if (fi > 0) { g = 1.0f - fi * step; fi--; }
                            float x = in[s] * g;
                            out[s] += x;
                            float a = fabsf(x);
                            if (a > peak) peak = a;
                        }
                        if (c == fVoiceOutputs - 1) { v.fFadeOut = fo; v.fFadeIn = fi; }
                    } else {
                        for (int s = 0; s < count; s++) {
                            out[s] += in[s];
                            float a = fabsf(in[s]);
                            if (a > peak) peak = a;
                        }
                    }
                }
                if (fadeOut > 0 && v.fFadeOut == 0) {
                    // faded out (the rest of the block was silent)
                    if (v.fNextNote != FREE) {
                        int note = v.fNextNote;
                        v.fNextNote = FREE;
                        start(v, note, v.fNextVel);
                        v.fFadeIn = fFade;
                    } else {
                        v.fNote = FREE;  // its new note was released before it started
                    }
                } else if (v.fFadeOut > 0) {
                    // still fading out
                } else if (!v.fHeld) {
                    v.fQuiet = peak < VOICE_STOP_LEVEL ? v.fQuiet + count : 0;
                    if (v.fQuiet >= getSampleRate() / 100) v.fNote = FREE;  // 10 ms quiet
                }
            }
            if (fEffect) fEffect->compute(count, fMix, outputs);
        }
};

#endif
