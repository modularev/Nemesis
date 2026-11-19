// SPDX-License-Identifier: GPL-3.0-or-later
declare name "superosc";
declare description "Drone generator: root, fourth and fifth, each as 4 detuned saw partials, through a ladder filter; MIDI note and V/Oct";
declare author "Arev Imer";
declare version "0.2";
declare options "[midi:on]";
declare license "GPL-3.0-or-later";

// Controls, modes and background: docs/superosc.md

import("stdfaust.lib");
import("nemesis.lib");

// ---- controls -------------------------------------------------------------

tune      = hslider("tune [hw:CV_X] [unit:st]", 0, -24, 24, 0.01) : si.smoo;
cutoff    = hslider("cutoff [hw:CV_Y] [scale:log] [unit:Hz]", 1200, 20, 18000, 1) : si.smoo;
resonance = hslider("resonance [hw:CV_Z]", 0.2, 0, 1, 0.01) : si.smoo;
voct      = hslider("v-oct [rev2:CV_A] [rev1:CV_E] [volts:1] [unit:V]", 0, -10, 10, 0.001) : si.smooth(ba.tau2pole(0.002));
root      = hslider("root [hw:POT_A]", 0.8, 0, 1, 0.01) : si.smoo;
fourth    = hslider("fourth [hw:POT_B] [cv:CV_B]", 0, 0, 1, 0.01) : si.smoo;
fifth     = hslider("fifth [hw:POT_C] [cv:CV_C]", 0.4, 0, 1, 0.01) : si.smoo;
detune    = hslider("detune [hw:POT_D] [cv:CV_D] [unit:Hz]", 0.5, 0, 5, 0.01) : si.smoo;

octave    = hslider("octave", 0, -2, 2, 1);
drift     = hslider("drift [rev1:CV_F] [midi:ctrl 1]", 0.2, 0, 1, 0.01) : si.smoo;
gated     = checkbox("gated");
swell     = hslider("swell [scale:log] [unit:s]", 1, 0.01, 10, 0.01);
level     = hslider("level", 0.8, 0, 1, 0.01) : si.smoo;

note      = hslider("note [mono:note]", 36, 0, 127, 1);       // C2 until a MIDI note arrives
gate      = hslider("gate [mono:gate]", 0, 0, 1, 1);
bend      = hslider("bend [midi:pitchwheel] [hidden:1]", 0, -2, 2, 0.001) : si.smoo;

// ---- pitch ----------------------------------------------------------------

semitones = note + 12 * octave + tune + bend + 12 * voct;
freq      = ba.midikey2hz(semitones);

// ---- oscillators ----------------------------------------------------------

RATIOS    = (1, 4/3, 3/2);   // root, fourth, fifth

// partial i (0..3) of group g (0..2); side +1 left, -1 right mirrors the detune
partial(side, g, i) = os.polyblep_saw(f) / (1 + 0.5 * i)
with {
    ratio  = ba.take(g + 1, RATIOS);
    spread = (i - 1.5) * (1 + 0.37 * g);                   // -1.5 .. 2.8, different per group
    // os.oscs: a sine without a table (os.osc reads a 256 kB table in PSRAM,
    // 24 times per sample at scattered positions)
    wander = drift * 0.004 * os.oscs(0.023 + 0.011 * (4 * g + i) + 0.007 * side);   // up to ~7 cents
    f      = (ratio * (i + 1) * freq + side * detune * spread) * (1 + wander);
};

group(side, g) = par(i, 4, partial(side, g, i)) :> _;
// the sum is scaled down once the three levels add up to more than the
// default (0.8 + 0.4), so all groups at full keep the limiter out of the way
voice(side)    = root * group(side, 0) + fourth * group(side, 1) + fifth * group(side, 2) : *(0.4 / max(1, (root + fourth + fifth) / 1.2));

// ---- filter, envelope, output -----------------------------------------------

// ladder filter: normFreq is log10(cutoff / 20) / 3, Q 0.7 .. 18; the gain
// lost at high resonance is partly made up
filter = ve.moogLadder(log10(cutoff / 20) / 3, q) : *(1 + 1.5 * resonance)
with {
    q = 0.707 + 17.3 * resonance;
};

// drone: always on; gated: MIDI gate, swelling in and out
envelope = select2(gated > 0.5, 1, gate : si.onePoleSwitching(swell, swell));

stereo = (voice(1) : filter), (voice(-1) : filter) : *(envelope * level), *(envelope * level) : nm_limiter2(0.9);

pitch_cv = (semitones - 60) / 12 / 5 : max(-1) : min(1);   // 1 V/oct, 0 V = C4, 1.0 = 5 V
gate_out = gate;

process = stereo, pitch_cv, gate_out;
