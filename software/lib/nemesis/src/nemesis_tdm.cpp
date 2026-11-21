// SPDX-License-Identifier: GPL-3.0-or-later
//
// Library for the programmable Eurorack DSP Module NEMESIS
//
// nemesis_tdm.cpp - direct TDM audio, see nemesis_tdm.h
//
// Copyright © 2021 Arev Imer if not mentioned elsewise
// released under GNU GPL v3 or later (see LICENSE)
//

#include "nemesis_tdm.h"
#include "nemesis_program.h" // outputCorrection()

namespace nemesis
{
   namespace
   {
      // the Teensy's DMA buffers hold two halves of AUDIO_BLOCK_SAMPLES (the
      // maximum); a smaller block uses their start
      constexpr uint32_t WORDS = AUDIO_BLOCK_SAMPLES * tdm::CHANNELS * 2;
      volatile int block = AUDIO_BLOCK_SAMPLES; // samples per half (per update)
      volatile uint32_t half_words = AUDIO_BLOCK_SAMPLES * tdm::CHANNELS;
      volatile int rate = (int)AUDIO_SAMPLE_RATE_EXACT; // set by AudioOutputTDM's config_tdm()

      // MCLK is 49.152 MHz at both rates (config_tdm made 24.576 MHz: the
      // SAI clock divider 14 becomes 7, PLL4 stays 688.128 MHz), so the codec
      // keeps its clock range (MFREQ 100) and needn't be powered down (that
      // popped on the DC coupled outputs). The bit clock (256 fs: 8 slots of
      // 32 bits) is MCLK / 4 at 48 kHz, MCLK / 2 at 96 kHz; the codec detects
      // the speed from MCLK / LRCK (1024 single, 512 double speed). The SAI
      // receiver makes the clocks (RCR2), the transmitter follows it. (The
      // i.MX RT SAI has no bit clock bypass for MCLK = 256 fs.)
      const int BASE_RATE = (int)AUDIO_SAMPLE_RATE_EXACT;
      bool mclk_doubled = false;
      void setDividers(int fs)
      {
         if (!mclk_doubled)
         {
            CCM_CS1CDR = (CCM_CS1CDR & ~(CCM_CS1CDR_SAI1_CLK_PRED_MASK | CCM_CS1CDR_SAI1_CLK_PODF_MASK)) |
                         CCM_CS1CDR_SAI1_CLK_PRED(1) | CCM_CS1CDR_SAI1_CLK_PODF(6);
            mclk_doubled = true;
         }
         uint32_t div = I2S_RCR2_DIV(fs == BASE_RATE ? 1 : 0); // / 4 or / 2
         I2S1_RCR2 = (I2S1_RCR2 & ~0xFFu) | div;
         I2S1_TCR2 = (I2S1_TCR2 & ~0xFFu) | div;
      }

      // the DMA buffers of AudioInputTDM/AudioOutputTDM (private there)
      uint32_t *rxBuffer = nullptr;
      uint32_t *txBuffer = nullptr;

      const int32_t *volatile ready = nullptr; // the receive half filled last
      int32_t *writing = nullptr;
      volatile uint32_t late = 0;

      // load: the cycles of the program's audio update (addLoad(), full 32
      // bit counts) and the blocks that passed. The library's figure is the
      // last block only, and its 16 bit count (cycles / 64) wraps at 4.19M
      // cycles: a 4.8M cycle frame read as a few %.
      volatile uint64_t load_sum = 0;
      volatile uint32_t load_peak = 0;
      volatile uint32_t load_count = 0;

      // test capture (startCapture): one input channel straight from the DMA
      EXTMEM int32_t capture_buf[tdm::CAPTURE_MAX];
      volatile int capture_channel = 0;
      volatile int capture_len = 0;
      volatile int capture_pos = 0;
      template <typename F> inline void measured(F update)   // F: the ISR's update_all() (protected)
      {
         load_count++;
         update();
      }
      volatile uint32_t written = 0;

      // the start of a DMA buffer: the current address minus how far the DMA
      // got in its major loop (4 bytes per minor loop)
      uint32_t *findBuffer(DMAChannel &dma, bool destination)
      {
         for (int tries = 0; tries < 10; tries++)
         {
            uint32_t citer = dma.TCD->CITER_ELINKNO;
            uint32_t address = destination ? (uint32_t)dma.TCD->DADDR : (uint32_t)dma.TCD->SADDR;
            uint32_t biter = dma.TCD->BITER_ELINKNO;
            if (citer != dma.TCD->CITER_ELINKNO)
               continue; // the DMA moved on while reading
            if (biter != WORDS)
               return nullptr;
            return (uint32_t *)(address - 4 * (biter - citer));
         }
         return nullptr;
      }
   }

   namespace tdm
   {
      const int32_t *input()
      {
         return ready;
      }

      int32_t *output()
      {
         if (!txBuffer)
            return nullptr;
         uint32_t saddr = (uint32_t)OutputTDM::channel().TCD->SADDR;
         writing = (int32_t *)((saddr < (uint32_t)(txBuffer + half_words)) ? txBuffer + half_words : txBuffer);
         return writing;
      }

      void outputDone()
      {
         if (!writing)
            return;
         arm_dcache_flush_delete(writing, half_words * 4);
         // the DMA already reads this half: it played (partly) old samples
         uint32_t now = (uint32_t)OutputTDM::channel().TCD->SADDR;
         if (now >= (uint32_t)writing && now < (uint32_t)(writing + half_words))
            late++;
         written++;
         writing = nullptr;
      }

      void startCapture(int channel, int samples)
      {
         __disable_irq();
         capture_channel = channel < 0 ? 0 : channel > CHANNELS - 1 ? CHANNELS - 1 : channel;
         capture_len = samples < 0 ? 0 : samples > tdm::CAPTURE_MAX ? tdm::CAPTURE_MAX : samples;
         capture_pos = 0;
         __enable_irq();
      }

      int captured(const int32_t *&samples)
      {
         samples = capture_buf;
         return capture_pos;
      }

      void addLoad(uint32_t cycles)
      {
         load_sum += cycles;
         if (cycles > load_peak)
            load_peak = cycles;
      }

      void takeLoad(uint64_t &sum, uint32_t &peak, uint32_t &count)
      {
         __disable_irq();
         sum = load_sum;
         peak = load_peak;
         count = load_count;
         load_sum = 0;
         load_peak = 0;
         load_count = 0;
         __enable_irq();
      }

      uint32_t lateWrites() { return late; }
      uint32_t writes() { return written; }

      int blockSize() { return block; }

      // the SAI stops at the end of a frame (transmitter first: it runs
      // synchronous to the receiver), both DMA channels get the new loop
      // length, the FIFOs are reset, the clock divider is set for the rate,
      // and receiver and transmitter start again together at slot 0, so the
      // channels keep their places
      FLASHMEM bool restart(int samples, int fs, bool force = false)
      {
         if (samples > AUDIO_BLOCK_SAMPLES || !rxBuffer || !txBuffer)
            return false;
         if (samples == block && fs == rate && !force)
            return true;
         DMAChannel &rx = InputTDM::channel();
         DMAChannel &tx = OutputTDM::channel();
         uint32_t words = samples * CHANNELS * 2; // both halves
         __disable_irq();
         I2S1_TCSR &= ~I2S_TCSR_TE;
         for (int i = 0; i < 100000 && (I2S1_TCSR & I2S_TCSR_TE); i++)
            ;
         I2S1_RCSR &= ~I2S_RCSR_RE;
         for (int i = 0; i < 100000 && (I2S1_RCSR & I2S_RCSR_RE); i++)
            ;
         rx.disable();
         tx.disable();
         rx.clearInterrupt();
         tx.clearInterrupt();
         rx.TCD->DADDR = rxBuffer;
         rx.TCD->CITER_ELINKNO = words;
         rx.TCD->BITER_ELINKNO = words;
         rx.TCD->DLASTSGA = -(int32_t)(words * 4);
         tx.TCD->SADDR = txBuffer;
         tx.TCD->CITER_ELINKNO = words;
         tx.TCD->BITER_ELINKNO = words;
         tx.TCD->SLAST = -(int32_t)(words * 4);
         memset(txBuffer, 0, WORDS * 4);
         arm_dcache_flush_delete(txBuffer, WORDS * 4);
         ready = nullptr;
         writing = nullptr;
         block = samples;
         half_words = samples * CHANNELS;
         if (fs != rate || force)
         {
            setDividers(fs);
            rate = fs;
         }
         I2S1_TCSR = I2S_TCSR_FR; // empty the FIFOs
         I2S1_RCSR = I2S_RCSR_FR;
         rx.enable();
         tx.enable();
         I2S1_RCSR = I2S_RCSR_RE | I2S_RCSR_BCE | I2S_RCSR_FRDE | I2S_RCSR_FR;
         I2S1_TCSR = I2S_TCSR_TE | I2S_TCSR_BCE | I2S_TCSR_FRDE;
         __enable_irq();
         return true;
      }

      FLASHMEM bool setBlockSize(int samples)
      {
         // 16 would leave a program only ~75 % of the block (the transmit DMA
         // reads a few frames ahead): too risky
         if (samples != 32 && samples != 64 && samples != 128)
            return false;
         return restart(samples, rate);
      }

      int sampleRate() { return rate; }
      // before the codec starts: MCLK 49.152 MHz (see setDividers())
      FLASHMEM bool begin() { return restart(block, rate, true); }
      // 48000 or 96000 (the same audio PLL); the codec must be told too
      FLASHMEM bool setSampleRate(int fs)
      {
         if (fs != BASE_RATE && fs != 2 * BASE_RATE)
            return false;
         return restart(block, fs);
      }
   }

   FLASHMEM InputTDM::InputTDM() : AudioInputTDM()
   {
      rxBuffer = findBuffer(dma, true);
      if (rxBuffer)
         dma.attachInterrupt(isr); // replaces AudioInputTDM's (copies into blocks)
   }

   // a receive half is full: hand it to the program and start the update
   void InputTDM::isr()
   {
      uint32_t daddr = (uint32_t)dma.TCD->DADDR;
      dma.clearInterrupt();
      uint32_t *done = (daddr < (uint32_t)(rxBuffer + half_words)) ? rxBuffer + half_words : rxBuffer;
      arm_dcache_delete(done, half_words * 4);
      ready = (const int32_t *)done;
      if (capture_pos < capture_len)
      {
         const int32_t *src = (const int32_t *)done + capture_channel;
         int n = block, p = capture_pos;
         if (n > capture_len - p)
            n = capture_len - p;
         for (int i = 0; i < n; i++)
            capture_buf[p + i] = src[i * tdm::CHANNELS];
         capture_pos = p + n;
      }
      if (update_responsibility)
         measured([] { update_all(); });
   }

   FLASHMEM OutputTDM::OutputTDM() : AudioOutputTDM()
   {
      txBuffer = findBuffer(dma, false);
      if (txBuffer)
         dma.attachInterrupt(isr); // replaces AudioOutputTDM's (copies from blocks)
   }

   // the transmit DMA started a half: clear the other one, the program
   // fills it in the update that follows (silence if no program runs)
   void OutputTDM::isr()
   {
      uint32_t saddr = (uint32_t)dma.TCD->SADDR;
      dma.clearInterrupt();
      uint32_t *next = (saddr < (uint32_t)(txBuffer + half_words)) ? txBuffer + half_words : txBuffer;
      // each output's calibrated zero (its offset), not digital 0: while no
      // program writes (a switch stops one and starts the next, ~7 ms) the
      // DC coupled outputs lost their offset, a DC pulse heard as a click
      int32_t zero[tdm::CHANNELS];
      for (int c = 0; c < tdm::CHANNELS; c++)
      {
         float scale, offset;
         outputCorrection(c, scale, offset);
         float v = offset * offsetGain * 2147483648.0f;
         zero[c] = v >= 2147483647.0f ? INT32_MAX : v <= -2147483648.0f ? INT32_MIN : (int32_t)v;
      }
      for (uint32_t i = 0; i < half_words; i += tdm::CHANNELS)
         for (int c = 0; c < tdm::CHANNELS; c++)
            next[i + c] = zero[c];
      arm_dcache_flush_delete(next, half_words * 4);
      if (update_responsibility)
         measured([] { update_all(); });
   }
}
