//
// FAUST .dsp file for the programmable Eurorack DSP Module NEMESIS by Arev Imer
//
// src/FAUST.dsp
//
// Copyright © 2021 Arev Imer if not mentioned elsewise
// released under GNU GPL v3
//

import("stdfaust.lib");

param_1 = hslider("POT X",0.5,0,1,0.001) : si.smoo;
param_2 = hslider("POT Y",0.5,0,1,0.001) : si.smoo;
param_3 = hslider("POT Z",0.5,0,1,0.001) : si.smoo;
param_4 = hslider("POT A",0.5,0,1,0.001) : si.smoo;
param_5 = hslider("POT B",0.5,0,1,0.001) : si.smoo;
param_6 = hslider("POT C",0.5,0,1,0.001) : si.smoo;
param_7 = hslider("POT D",0.5,0,1,0.001) : si.smoo;
param_8 = hslider("POT E",0.5,0,1,0.001) : si.smoo;
param_9 = hslider("POT F",0.5,0,1,0.001) : si.smoo;

process = _,_,_,_,_,_,_,_;