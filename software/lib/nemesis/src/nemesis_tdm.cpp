//
// Library for the programmable Eurorack DSP Module NEMESIS
//
// nemesis_tdm.cpp - direct TDM audio, see nemesis_tdm.h
//
// Copyright © 2021 Arev Imer if not mentioned elsewise
// released under GNU GPL v3
//

#include "nemesis_tdm.h"

namespace nemesis
{
   namespace
   {
      constexpr uint32_t WORDS = AUDIO_BLOCK_SAMPLES * tdm::CHANNELS * 2; // both halves
      constexpr uint32_t HALF_BYTES = WORDS * 2;

      // the DMA buffers of AudioInputTDM/AudioOutputTDM (private there)
      uint32_t *rxBuffer = nullptr;
      uint32_t *txBuffer = nullptr;

      const int32_t *volatile ready = nullptr; // the receive half filled last
      int32_t *writing = nullptr;
      volatile uint32_t late = 0;
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
         writing = (int32_t *)((saddr < (uint32_t)(txBuffer + WORDS / 2)) ? txBuffer + WORDS / 2 : txBuffer);
         return writing;
      }

      void outputDone()
      {
         if (!writing)
            return;
         arm_dcache_flush_delete(writing, HALF_BYTES);
         // the DMA already reads this half: it played (partly) old samples
         uint32_t now = (uint32_t)OutputTDM::channel().TCD->SADDR;
         if (now >= (uint32_t)writing && now < (uint32_t)(writing + WORDS / 2))
            late++;
         written++;
         writing = nullptr;
      }

      uint32_t lateWrites() { return late; }
      uint32_t writes() { return written; }
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
      uint32_t *done = (daddr < (uint32_t)(rxBuffer + WORDS / 2)) ? rxBuffer + WORDS / 2 : rxBuffer;
      arm_dcache_delete(done, HALF_BYTES);
      ready = (const int32_t *)done;
      if (update_responsibility)
         update_all();
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
      uint32_t *next = (saddr < (uint32_t)(txBuffer + WORDS / 2)) ? txBuffer + WORDS / 2 : txBuffer;
      memset(next, 0, HALF_BYTES);
      arm_dcache_flush_delete(next, HALF_BYTES);
      if (update_responsibility)
         update_all();
   }
}
