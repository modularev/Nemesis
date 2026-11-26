// SPDX-License-Identifier: GPL-3.0-or-later
declare name "thru";
declare description "Inputs 1-8 to outputs 1-8, unity: checks levels and the input and output scaling (5 V = 1.0)";
declare author "Arev Imer";
declare version "0.1";
declare audio_outputs "none";   // no output stage (soft clip etc.): the signal as it comes in
declare license "GPL-3.0-or-later";

// Controls and background: docs/thru.md

process = par(i, 8, _);
