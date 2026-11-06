//
// Library for the programmable Eurorack DSP Module NEMESIS
//
// nemesis_tdm.h
//
// Direct TDM audio for the CS42448: the running program reads its inputs
// from the receive DMA buffer and writes its outputs into the transmit DMA
// buffer, as 32 bit words (24 bit audio, left justified), 8 per frame, one
// per channel. No Audio library blocks: no splitting into 16 bit MSB/LSB
// blocks, no queues, no repacking, (almost) no AudioMemory.
//
// Round trip: two audio blocks (256 samples, 5.3 ms at 48 kHz) plus the
// codec, instead of four with AudioInputTDM/AudioOutputTDM:
//
//  - the receive DMA interrupt marks the half just filled and starts the
//    update, so a program always reads this cycle's input (with the Audio
//    library's objects it got the blocks one update later: the update order
//    follows construction, the programs come first);
//  - the program writes the half of the transmit buffer that plays next
//    (AudioOutputTDM copies at the next interrupt, one block later).
//
// The transmit DMA keeps the SAI FIFO full, so it finishes its half a few
// frames before the receive DMA: at update time the output plays its new
// half and the other one has a block of time to be filled (the program may
// use up to ~97 % CPU). tdm::lateWrites() counts blocks written too late.
//
// AudioInputTDM/AudioOutputTDM still set up the SAI, the clocks and the DMA;
// the subclasses here take over the DMA interrupts.
//
// Copyright © 2021 Arev Imer if not mentioned elsewise
// released under GNU GPL v3
//

#ifndef nemesis_tdm_h
#define nemesis_tdm_h

#include <Audio.h>

namespace nemesis
{
   namespace tdm
   {
      constexpr int CHANNELS = 8; // words per frame

      // this cycle's input: AUDIO_BLOCK_SAMPLES frames of CHANNELS words,
      // null before the first block
      const int32_t *input();
      // the output half that plays next, zeroed by the interrupt; call
      // outputDone() after writing it
      int32_t *output();
      void outputDone();

      uint32_t lateWrites();
      uint32_t writes();
   }

   class InputTDM : public AudioInputTDM
   {
   public:
      InputTDM();
      void update() override {}

   private:
      static void isr();
   };

   class OutputTDM : public AudioOutputTDM
   {
   public:
      OutputTDM();
      void update() override {}
      static DMAChannel &channel() { return dma; }

   private:
      static void isr();
   };
}

#endif
