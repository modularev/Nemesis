//
// Library for the programmable Eurorack DSP Module NEMESIS
//
// nemesis_transport.cpp
//
// MIDI input and the transport (both hardware revisions).
//
// The MIDI jack (Serial1) and USB MIDI are read in a timer interrupt every
// 100 us, below the audio priority. Clock, Start, Continue, Stop and Song
// Position go to the transport with a timestamp from the CPU cycle counter;
// everything else (and a copy of the real time messages, for FAUST's
// [midi:clock]) is queued for the program's MIDI handler.
//
// The transport (nemesis_transport_core.h) advances once per audio block,
// called by the program before it computes the block. With the clock source
// CV, rising edges on audio IN 8 are the clock, found in its samples, so
// they are timed to the sample; MIDI clock and Start/Stop are ignored.
//
// Copyright © 2021 Arev Imer if not mentioned elsewise
// released under GNU GPL v3
//

#include "nemesis.h"
#include "nemesis_transport.h"
#include "nemesis_transport_core.h"
#include "nemesis_tdm.h"

namespace nemesis
{
   namespace
   {
      constexpr uint32_t POLL_US = 100;
      constexpr uint8_t POLL_PRIORITY = 224; // below the audio update (208)

      IntervalTimer poll_timer;

      // single producer (poll interrupt), single consumer (audio update) queues
      template <typename T, int N>
      struct Queue
      {
         T items[N];
         volatile uint32_t head = 0, tail = 0;
         bool push(const T &item)
         {
            uint32_t h = head;
            if (h - tail >= N)
               return false; // full: dropped
            items[h % N] = item;
            asm volatile("" ::: "memory");
            head = h + 1;
            return true;
         }
         bool pop(T &item)
         {
            uint32_t t = tail;
            if (t == head)
               return false;
            item = items[t % N];
            asm volatile("" ::: "memory");
            tail = t + 1;
            return true;
         }
      };

      struct TransportEvent
      {
         uint8_t type; // 0xF8 clock, 0xFA start, 0xFB continue, 0xFC stop, 0xF2 song position
         uint16_t value;
         uint32_t cycles;
      };

      Queue<MidiMessage, 256> messages;
      Queue<TransportEvent, 64> events;

      volatile uint32_t serial_bytes = 0, usb_messages = 0, dropped = 0;
      volatile uint8_t last_status = 0;

      // serial MIDI parser state (poll interrupt only)
      uint8_t running_status = 0;
      uint8_t data[2];
      int data_count = 0;

      TransportCore core;
      bool last_run_setting = true;
      double block_time = 0; // samples since the transport started

      void queueMessage(uint8_t status, uint8_t d1, uint8_t d2)
      {
         if (!messages.push({status, d1, d2}))
            dropped = dropped + 1;
      }

      void realtime(uint8_t status, uint32_t cycles)
      {
         if (status == 0xF8 || status == 0xFA || status == 0xFB || status == 0xFC)
            events.push({status, 0, cycles});
         queueMessage(status, 0, 0);
      }

      int dataLength(uint8_t status)
      {
         if (status == 0xF2)
            return 2;
         if (status == 0xF1 || status == 0xF3)
            return 1;
         switch (status & 0xF0)
         {
         case 0xC0:
         case 0xD0:
            return 1;
         default:
            return 2;
         }
      }

      // running status parser; SysEx and other system common messages are skipped
      void parse(uint8_t byte, uint32_t cycles)
      {
         if (byte >= 0xF8)
         {
            realtime(byte, cycles);
            return;
         }
         if (byte & 0x80)
         {
            last_status = byte;
            running_status = (byte == 0xF2 || byte == 0xF1 || byte == 0xF3 || byte < 0xF0) ? byte : 0;
            data_count = 0;
            return;
         }
         if (!running_status)
            return;
         data[data_count++] = byte;
         int length = dataLength(running_status);
         if (data_count < length)
            return;
         data_count = 0;
         if (running_status == 0xF2)
            events.push({0xF2, (uint16_t)(data[0] | (data[1] << 7)), cycles});
         else if (running_status < 0xF0)
            queueMessage(running_status, data[0], length > 1 ? data[1] : 0);
         if (running_status >= 0xF0)
            running_status = 0; // system common messages have no running status
      }

      void poll()
      {
         uint32_t cycles = ARM_DWT_CYCCNT;
         while (Serial1.available())
         {
            serial_bytes = serial_bytes + 1;
            parse(Serial1.read(), cycles);
         }
#if defined(MIDI_INTERFACE)
         while (usbMIDI.read())
         {
            usb_messages = usb_messages + 1;
            uint8_t type = usbMIDI.getType();
            if (type >= 0xF8)
               realtime(type, cycles);
            else if (type == 0xF2) // usbMIDI.SongPosition
               events.push({0xF2, (uint16_t)(usbMIDI.getData1() | (usbMIDI.getData2() << 7)), cycles});
            else if (type >= 0x80 && type < 0xF0)
               queueMessage(type | ((usbMIDI.getChannel() - 1) & 0x0F), usbMIDI.getData1(), usbMIDI.getData2());
         }
#endif
      }
   }

   namespace transport
   {
      FLASHMEM void begin()
      {
         Serial1.begin(31250);
         last_run_setting = systemSettings().run;
         core.setRunning(last_run_setting);
         poll_timer.priority(POLL_PRIORITY);
         poll_timer.begin(poll, POLL_US);
      }

      // clock on IN 8: rising edges with hysteresis, in input full scale
      // (+-2.77 V at the jack, a 1 Hz high-pass in front of the codec)
      constexpr int CLOCK_CHANNEL = 7;
      constexpr float CLOCK_HIGH = 0.3f, CLOCK_LOW = 0.15f;
      bool clock_high = false;

      void scanClockInput(double block_start)
      {
         const int32_t *frames = tdm::input();
         if (!frames)
            return;
         // this block's input was recorded during the previous block
         float scale = 4.656612873077392578125e-10f * inputScale(CLOCK_CHANNEL);
         int n = tdm::blockSize();
         for (int i = 0; i < n; i++)
         {
            float v = frames[i * tdm::CHANNELS + CLOCK_CHANNEL] * scale;
            if (!clock_high && v > CLOCK_HIGH)
            {
               clock_high = true;
               core.clock(block_start - n + i);
            }
            else if (clock_high && v < CLOCK_LOW)
               clock_high = false;
         }
      }

      // once per audio block, before the program computes it
      const TransportState &block()
      {
         const double CYCLES_PER_SAMPLE = (double)F_CPU_ACTUAL / AUDIO_SAMPLE_RATE_EXACT;
         uint32_t now = ARM_DWT_CYCCNT;

         SystemSettings &s = systemSettings();
         core.sampleRate = AUDIO_SAMPLE_RATE_EXACT;
         core.source = s.clockSource;
         bool cv = s.clockSource == TransportCore::CV;
         core.tickSize = cv ? TransportCore::TICKS_PER_QUARTER / max(1, s.clockPpqn) : 1.0;
         core.tempo = s.tempo;
         core.offsetSamples = s.clockOffsetMs * 0.001 * AUDIO_SAMPLE_RATE_EXACT;
         if (s.run != last_run_setting) // run switched in the menu
            core.setRunning(s.run);

         TransportEvent e;
         while (events.pop(e))
         {
            double t = block_time - (double)(uint32_t)(now - e.cycles) / CYCLES_PER_SAMPLE;
            if (cv)
               continue; // following IN 8: MIDI clock, Start/Stop and Song Position don't count
            switch (e.type)
            {
            case 0xF8:
               core.clock(t);
               break;
            case 0xFA:
               core.start();
               break;
            case 0xFB:
               core.cont();
               break;
            case 0xFC:
               core.stop();
               break;
            case 0xF2:
               core.songPosition(e.value);
               break;
            }
         }
         if (cv)
            scanClockInput(block_time);
         // MIDI Start/Stop show in the menu (not stored)
         s.run = last_run_setting = core.isRunning();

         int n = tdm::blockSize();
         const TransportState &state = core.block(block_time, n);
         block_time += n;
         return state;
      }
   }

   const TransportState &transportState() { return core.current(); }

   bool midiRead(MidiMessage &message) { return messages.pop(message); }

   FLASHMEM void printMidiStatus(Print &out)
   {
      const TransportState &s = core.current();
      out.printf("MIDI: %lu bytes on the MIDI jack (last status 0x%02X), %lu USB messages, %lu dropped\n",
                 (unsigned long)serial_bytes, last_status, (unsigned long)usb_messages, (unsigned long)dropped);
      out.printf("transport: %s %.1f BPM, %s, position %.2f ticks\n", s.cvClock ? "IN 8" : s.external ? "MIDI" : "internal",
                 s.bpm, s.running ? "running" : "stopped", s.position);
   }
}
