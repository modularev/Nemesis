// SPDX-License-Identifier: GPL-3.0-or-later
//
// main file for the programmable Eurorack DSP Module NEMESIS by Arev Imer
//
// src/main.cpp
//
// Copyright © 2021 Arev Imer if not mentioned elsewise
// released under GNU GPL v3 or later (see LICENSE)
//

#include <nemesis.h>
#include "myProgram.h" // generated from src/myProgram.dsp at build time

myProgram program;

void setup()
{
   nemesis::begin(program);
   nemesis::printControls();
}

void loop()
{
   nemesis::update();
}
