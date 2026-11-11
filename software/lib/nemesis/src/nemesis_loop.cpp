//
// Library for the programmable Eurorack DSP Module NEMESIS
//
// nemesis_loop.cpp - a program's frozen loop memory on the SD card, see
// nemesis_loop.h
//
// Copyright © 2021 Arev Imer if not mentioned elsewise
// released under GNU GPL v3
//

#include "nemesis_loop.h"
#include "nemesis.h"
#include <SD.h>

namespace nemesis
{
   namespace loop
   {
      namespace
      {
         constexpr int MAX_BUFFERS = 4;
         constexpr int CHUNK = 256;           // frames per SD write
         constexpr uint32_t STABLE_MS = 500;  // frozen this long before saving
         constexpr uint32_t MARGIN = 2 * AUDIO_BLOCK_SAMPLES;
         const char DIRECTORY[] = "/NEMESIS";

         // state of the running program's loop
         const Program *watched = nullptr;
         uint32_t frozenLength = 0;
         elapsedMillis frozenFor;
         bool saved = false;

         void path(const Program &program, char *text, size_t size)
         {
            snprintf(text, size, "%s/%s.WAV", DIRECTORY, program.name());
         }

         void put32(uint8_t *p, uint32_t v)
         {
            p[0] = v;
            p[1] = v >> 8;
            p[2] = v >> 16;
            p[3] = v >> 24;
         }
         void put16(uint8_t *p, uint16_t v)
         {
            p[0] = v;
            p[1] = v >> 8;
         }
         uint32_t get32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
         uint16_t get16(const uint8_t *p) { return p[0] | p[1] << 8; }

         // a canonical 44 byte WAV header, IEEE float (format 3)
         void header(uint8_t *h, int channels, uint32_t frames)
         {
            uint32_t bytes = frames * channels * 4;
            memcpy(h, "RIFF", 4);
            put32(h + 4, 36 + bytes);
            memcpy(h + 8, "WAVEfmt ", 8);
            put32(h + 16, 16);
            put16(h + 20, 3);
            put16(h + 22, channels);
            put32(h + 24, (uint32_t)sampleRate());
            put32(h + 28, (uint32_t)sampleRate() * channels * 4);
            put16(h + 32, channels * 4);
            put16(h + 34, 32);
            memcpy(h + 36, "data", 4);
            put32(h + 40, bytes);
         }

         // the loop's frames, oldest first: frame k of a loop of L samples
         // that ended with sample `end` (samples computed) is at
         // (end - L + k) mod length in every delay line
         FLASHMEM bool save(Program &program, uint32_t frames)
         {
            float *buffers[MAX_BUFFERS];
            uint32_t lengths[MAX_BUFFERS];
            int channels = program.largeBuffers(buffers, lengths, MAX_BUFFERS);
            if (channels <= 0)
               return false;
            for (int c = 0; c < channels; c++)
               if (frames + MARGIN >= lengths[c])
                  return false;

            char name[48];
            path(program, name, sizeof(name));
            if (!SD.exists(DIRECTORY))
               SD.mkdir(DIRECTORY);
            SD.remove(name);
            File file = SD.open(name, FILE_WRITE);
            if (!file)
               return false;

            uint32_t start = millis();
            uint8_t h[44];
            header(h, channels, frames);
            file.write(h, sizeof(h));

            const uint64_t end = program.samplesComputed();
            static float chunk[CHUNK * MAX_BUFFERS];
            bool ok = true;
            for (uint32_t k = 0; k < frames && ok; k += CHUNK)
            {
               watchdogFeed(); // seconds of SD writing for a long loop
               uint32_t n = min((uint32_t)CHUNK, frames - k);
               // the program overwrites frame k after (length - frames + k)
               // more samples: stop if it got close
               uint32_t ran = (uint32_t)(program.samplesComputed() - end);
               for (int c = 0; c < channels; c++)
                  if (ran + MARGIN >= lengths[c] - frames + k)
                     ok = false;
               if (!ok)
                  break;
               for (int c = 0; c < channels; c++)
               {
                  uint32_t len = lengths[c];
                  uint32_t i = (uint32_t)((end + len - frames + k) % len);
                  for (uint32_t f = 0; f < n; f++)
                  {
                     chunk[f * channels + c] = buffers[c][i];
                     if (++i == len)
                        i = 0;
                  }
               }
               size_t bytes = n * channels * 4;
               ok = file.write((const uint8_t *)chunk, bytes) == bytes;
            }
            file.close();
            if (!ok)
               SD.remove(name);
            Serial.printf("loop: %s %s, %lu frames x %d in %lu ms\n", ok ? "saved" : "not saved", name,
                          (unsigned long)frames, channels, (unsigned long)(millis() - start));
            return ok;
         }
      }

      void service(Program &program)
      {
         float *zone = program.loopLength();
         if (&program != watched)
         {
            // another program: what restore() set up belongs to it
            watched = &program;
            frozenLength = 0;
            saved = program.loopRestored() && *program.loopRestored() > 0;
         }
         if (!zone)
            return;
         uint32_t length = (uint32_t)*zone;
         if (length != frozenLength)
         {
            // frozen anew, or the loop length changed
            if (frozenLength != 0 || length == 0)
               saved = false;
            frozenLength = length;
            frozenFor = 0;
         }
         if (length == 0)
         {
            // recording again: a loop loaded at start no longer applies
            if (program.loopRestored())
               *program.loopRestored() = 0;
            return;
         }
         if (!saved && frozenFor >= STABLE_MS)
         {
            save(program, length);
            saved = true; // also after a failure: once per freeze
         }
      }

      FLASHMEM void restore(Program &program)
      {
         if (!program.loopRestored() || !program.loopFreeze() || *program.loopFreeze() < 0.5f)
            return;
         float *buffers[MAX_BUFFERS];
         uint32_t lengths[MAX_BUFFERS];
         int channels = program.largeBuffers(buffers, lengths, MAX_BUFFERS);
         if (channels <= 0)
            return;

         char name[48];
         path(program, name, sizeof(name));
         File file = SD.open(name, FILE_READ);
         if (!file)
            return;
         uint32_t start = millis();
         uint8_t h[44];
         bool ok = file.read(h, sizeof(h)) == sizeof(h) && memcmp(h, "RIFF", 4) == 0 && get16(h + 20) == 3 &&
                   get16(h + 22) == channels && get16(h + 34) == 32 && memcmp(h + 36, "data", 4) == 0;
         // a loop saved at the other sample rate loads too (the buffers are
         // the same in samples): it plays an octave up or down
         uint32_t frames = ok ? get32(h + 40) / (channels * 4) : 0;
         for (int c = 0; c < channels && ok; c++)
            if (frames == 0 || frames + MARGIN >= lengths[c])
               ok = false;

         // the program has not run yet: each delay line writes its next
         // sample at 0, so the newest saved frame goes to length - 1
         static float chunk[CHUNK * MAX_BUFFERS];
         for (uint32_t k = 0; k < frames && ok; k += CHUNK)
         {
            watchdogFeed();
            uint32_t n = min((uint32_t)CHUNK, frames - k);
            size_t bytes = n * channels * 4;
            if (file.read((uint8_t *)chunk, bytes) != (int)bytes)
            {
               ok = false;
               break;
            }
            for (int c = 0; c < channels; c++)
            {
               float *buffer = buffers[c];
               uint32_t i = lengths[c] - frames + k;
               for (uint32_t f = 0; f < n; f++)
                  buffer[i + f] = chunk[f * channels + c];
            }
         }
         file.close();
         if (ok)
            *program.loopRestored() = frames;
         else
            for (int c = 0; c < channels; c++)
               memset(buffers[c], 0, lengths[c] * sizeof(float));
         watched = &program;
         frozenLength = 0;
         saved = ok; // don't write the same loop back
         Serial.printf("loop: %s %s, %lu frames x %d in %lu ms\n", ok ? "loaded" : "could not load", name,
                       (unsigned long)frames, channels, (unsigned long)(millis() - start));
      }
   }
}
