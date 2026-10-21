//
// FAUST .dsp file for the programmable Eurorack DSP Module NEMESIS by Arev Imer
//
// src/myProgram.dsp - the file name becomes the C++ class name (myProgram.h)
//
// Copyright © 2021 Arev Imer if not mentioned elsewise
// released under GNU GPL v3
//
// Parameters are bound to the hardware inputs (values 0..1 scaled to the
// slider range) in one of three ways:
//
//   hslider("CV_X: Gain", ...)                 input name as label prefix
//   hslider("Gain [hw:CV_X]", ...)             metadata for every revision
//   hslider("Gain [rev1:CV_C] [rev2:POT_A]")   metadata per revision
//
//   rev1 inputs: CV_X CV_Y CV_Z CV_A .. CV_F (knob + CV, POT_X .. POT_F also work)
//   rev2 inputs: CV_X CV_Y CV_Z CV_A CV_B CV_C CV_D and POT_A POT_B POT_C POT_D
//
// Unbound parameters keep their default value. Options for MIDI or
// polyphony: declare options "[midi:on][nvoices:8]";

import("stdfaust.lib");

gain   = hslider("Gain [hw:CV_X]", 0.5, 0, 1, 0.001) : si.smoo;
cutoff = hslider("Cutoff [rev1:CV_A] [rev2:POT_A]", 0.8, 0, 1, 0.001) : si.smoo : *(9900) : +(100);

process = par(i, 2, fi.lowpass(2, cutoff) : *(gain));
