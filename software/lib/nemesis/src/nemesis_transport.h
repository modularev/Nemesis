//
// Library for the programmable Eurorack DSP Module NEMESIS
//
// nemesis_transport.h
//
// Internal interface of MIDI input and transport (nemesis_transport.cpp),
// the public part is in nemesis.h.
//
// Copyright © 2021 Arev Imer if not mentioned elsewise
// released under GNU GPL v3
//

#ifndef nemesis_transport_h
#define nemesis_transport_h

#include "nemesis.h"

namespace nemesis
{
   namespace transport
   {
      // opens the MIDI jack and starts reading MIDI
      void begin();
   }
}

#endif
