// SPDX-License-Identifier: GPL-3.0-or-later
declare name "slush";
declare description "Spectral freeze and smear (a phase vocoder): holds the input as an endless sound that keeps moving, or smears it continuously; drift, crossfades";
declare author "Arev Imer";
declare version "0.3";
declare audio_outputs "1-4";
declare block_size "128";        // a frame (2048 points, smear on) takes ~80 % of a 64 sample block: ~40 % of 128
declare spectral "2";            // two channels of spectral processing (nemesis_spectral.h)
declare license "GPL-3.0-or-later";

// Controls, modes and background: docs/slush.md

import("stdfaust.lib");
import("nemesis.lib");

spectral = ffunction(float nm_spectral(int, int, int, float, float, float, float, float, float, float, float), "nemesis_spectral.h", "");
frozen_level = ffunction(float nm_spectral_level(int, float), "nemesis_spectral.h", "");
FREEZE   = 0;      // the kernel
SIZE     = select2(res_k, 1024, 2048) : int;   // FFT points (the library reallocates when it changes)
HOP      = SIZE / 4;   // samples per frame
ALIGN    = select2(res_k, 192, 640) : int;   // the wet sound follows its level this much
                   // later (measured on the Mac: a crossfade from a sine to its frozen
                   // copy stays within 0.6 dB; at 1024: 0 dipped 1.3 dB, 512 bumped
                   // 1.6 dB; at 2048: 384 dipped 1.6 dB)

// ---- controls -------------------------------------------------------------------

drift   = hslider("drift [hw:CV_X]", 0.1, 0, 1, 0.001);
motion  = hslider("motion [hw:CV_Y]", 0.4, 0, 1, 0.001);
fade_k  = hslider("fade [hw:CV_Z]", 0.3, 0, 1, 0.001);
mix_k   = hslider("mix [hw:POT_A]", 0.5, 0, 1, 0.01);
level_k = hslider("level [hw:POT_B] [cv:CV_B]", 0.5, 0, 1, 0.01);
tone    = hslider("tone [hw:POT_C] [cv:CV_C]", 0.5, 0, 1, 0.01);
smear_k = hslider("smear [hw:POT_D] [cv:CV_D]", 0, 0, 1, 0.001);
speed_k = hslider("[01] speed", 0.3, 0, 1, 0.001);
tilt    = hslider("[02] tilt", 0, -1, 1, 0.01);
res_k   = hslider("[03] resolution [style:menu{'1024':0;'2048':1}]", 1, 0, 1, 1) > 0.5;
width   = hslider("[04] width", 1, 0, 1, 0.01) : si.smoo;
btn     = button("freeze [button:1]");
gate    = hslider("freeze cv [rev2:CV_A] [rev1:CV_E]", 0, 0, 1, 0.001) > 0.5;

// button 1 toggles the freeze; a gate on CV A (rev1: knob/CV E) freezes
// while high, and every rising edge captures anew
toggle(b) = (\(s).(select2(b > b', s, 1 - s))) ~ _;
frozen    = toggle(btn) | gate;
rise      = (frozen > frozen') | (gate > gate');
captures  = (+(rise) : %(100000)) ~ _;
a         = select2(frozen, 0, 1 + captures);          // the kernel captures when it changes

// fade 0.02 .. 8 s, as a step per frame (from the raw knob: no pow per sample)
fade_s    = 0.02 * pow(400, fade_k);
step      = HOP / (fade_s * ma.SR);
// motion speed: back and forth 0.05 .. 2 times a second whatever the span
// (as frames per frame: span 2 x motion x 31 frames, a frame every HOP
// samples; in frames per frame, full speed over a short span moved ~19
// times a second: rough); per channel slightly different, so left and right
// move apart
rate(c)   = 0.05 * pow(40, speed_k) * (1 + 0.13 * c);
// smear: off at the left end, then 0.05 .. 20 s
smear_s   = select2(smear_k < 0.02, 0.05 * pow(400, (smear_k - 0.02) / 0.98), 0);
speed(c)  = 2 * motion * 31 * rate(c) * HOP / ma.SR;

// tone as pollen's: state variable filters (they stay smooth when the
// cutoff jumps), cutoffs from the raw knob
tt        = tone * 2 - 1;
tone_f    = fi.svf.lp(20000 * 0.01 ^ max(0, -tt), 0.7071) : fi.svf.hp(20 * 100 ^ max(0, tt), 0.7071);

level     = level_k * 2 : si.smoo;
wet_g     = sin(mix_k * ma.PI / 2) : si.smoo;
dry_g     = cos(mix_k * ma.PI / 2) : si.smoo;
// the original: full while nothing is frozen; as the frozen sound fades in
// (g, from the library, aligned with what sounds and smoothed) it fades to
// the mix's dry share: fully wet crossfades from the original to the
// frozen sound and back
dry_of(g) = 1 - g * (1 - dry_g);
aligned(g) = g @ ALIGN : si.smooth(ba.tau2pole(0.005));

// ---- process ------------------------------------------------------------------

// out 1/2: the original crossfading into the frozen sound (the original
// never goes through the FFT: no comb filter), out 3/4: the frozen sound
// alone
process(inl, inr) = ol, or, fl, fr
with {
    xl  = spectral(0, FREEZE, SIZE, inl, a, step, drift, smear_s, motion, speed(0), tilt);
    xr  = spectral(1, FREEZE, SIZE, inr, a, step, drift, smear_s, motion, speed(1), tilt);
    gl  = frozen_level(0, xl) : aligned;
    gr  = frozen_level(1, xr) : aligned;
    sl  = xl : tone_f : *(level);
    sr  = xr : tone_f : *(level);
    m   = (sl + sr) * 0.5;
    s   = (sl - sr) * 0.5 * width;
    fl  = m + s;
    fr  = m - s;
    ol  = inl * dry_of(gl) + fl * wet_g : nm_softclip(0.9) : attach(_, frozen : vbargraph("frozen [led:1]", 0, 1));
    or  = inr * dry_of(gr) + fr * wet_g : nm_softclip(0.9);
};
