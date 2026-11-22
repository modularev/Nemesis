// SPDX-License-Identifier: GPL-3.0-or-later
declare name "slush";
declare description "Spectral freeze and smear (a phase vocoder): holds the input as an endless sound that keeps moving, or smears it continuously; glides between freezes, drift, purity, layers, pitch, grain";
declare author "Arev Imer";
declare version "0.5";
declare audio_outputs "1-4";
declare block_size "128";        // a frame (2048 points, smear on) takes ~80 % of a 64 sample block: ~40 % of 128
declare spectral "2";            // two channels of spectral processing (nemesis_spectral.h)
declare license "GPL-3.0-or-later";

// Controls, modes and background: docs/slush.md

import("stdfaust.lib");
import("nemesis.lib");

spectral = ffunction(float nm_spectral(int, int, int, float, float, float, float, float, float, float, float, float, float, float, float, float, float, float), "nemesis_spectral.h", "");
frozen_level = ffunction(float nm_spectral_level(int, float), "nemesis_spectral.h", "");
FREEZE   = 0;      // the kernel
SIZE     = 512, 1024, 2048, 4096 : ba.selectn(4, res_k) : int;   // FFT points (the library reallocates when it changes)
HOP      = SIZE / 4;   // samples per frame
ALIGN    = SIZE * 7 / 16;   // the wet sound follows its level this much later: half a
                   // window, less the synthesis's N/16 delay (measured on the Mac with
                   // a 20 ms fade, 12 freeze moments: 2048 best at 896, 512 at 128-224,
                   // 1024 and 4096 within a dB of their best; the old 192/640 dated from
                   // before the frames ran in halves)

// ---- controls -------------------------------------------------------------------

drift   = hslider("drift [hw:CV_X]", 0.1, 0, 1, 0.001);
scatter = hslider("scatter [hw:CV_Y]", 0, 0, 1, 0.001);
speed_k = hslider("speed [hw:CV_Z]", 0.3, 0, 1, 0.001);
mix_k   = hslider("mix [hw:POT_A]", 0.5, 0, 1, 0.01);
level_k = hslider("level [hw:POT_B] [cv:CV_B]", 0.5, 0, 1, 0.01);
purity  = hslider("purity [hw:POT_C] [cv:CV_C]", 0, 0, 1, 0.001);
smear_k = hslider("smear [hw:POT_D] [cv:CV_D]", 0, 0, 1, 0.001);
scatter_s = hslider("[01] scatter time [unit:s]", 0.3, 0.05, 3, 0.01);
tilt    = hslider("[02] tilt", 0, -1, 1, 0.01);
res_k   = hslider("[03] resolution [style:menu{'512':0;'1024':1;'2048':2;'4096':3}]", 2, 0, 3, 1) : int;
width   = hslider("[04] width", 1, 0, 1, 0.01) : si.smoo;
tone    = hslider("[05] tone", 0.5, 0, 1, 0.01);
layer   = hslider("[06] layer", 0, 0, 1, 0.01);
grain   = hslider("[07] grain", 0, 0, 1, 0.01);
lock    = hslider("[09] lock", 1, 0, 1, 0.01);
pitch   = hslider("[08] pitch [unit:st]", 0, -24, 24, 1);
btn     = button("freeze [button:1]");
gate    = hslider("freeze cv [rev2:CV_A] [rev1:CV_E]", 0, 0, 1, 0.001) > 0.5;

// button 1 toggles the freeze; a gate on CV A (rev1: knob/CV E) freezes
// while high, and every rising edge captures anew
toggle(b) = (\(s).(select2(b > b', s, 1 - s))) ~ _;
frozen    = toggle(btn) | gate;
rise      = (frozen > frozen') | (gate > gate');
captures  = (+(rise) : %(100000)) ~ _;
a         = select2(frozen, 0, 1 + captures);          // the kernel captures when it changes

// speed: how long a new freeze takes to glide from the sound before (every
// partial's level and pitch; a release fades out as long): 0 at the left
// end (at once), then 0.02 .. 60 s; as a step per frame (from the raw knob:
// no pow per sample)
speed_s   = select2(speed_k < 0.01, 0.02 * pow(3000, (speed_k - 0.01) / 0.99), 0);
step      = select2(speed_s > 0, 1, HOP / (max(speed_s, 1e-6) * ma.SR));
morph     = 1;   // the glide: new partials from the old ones' pitch and phase
// smear: always on (the mix decides how much is heard): 0 at the left end
// (no inertia: the input resynthesized), then 0.05 .. 20 s
smear_amt = 1;
smear_s   = select2(smear_k < 0.01, 0.05 * pow(400, (smear_k - 0.01) / 0.99), 0);
// pitch of the frozen sound as a ratio (from the raw setting: no pow per
// sample; the library glides it)
ratio     = pow(2, pitch / 12);

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
// frozen sound and back. Equal power (the frozen sound comes in with g in
// amplitude, the original goes with sqrt(1 - g^2)): the two aren't in
// phase, a linear crossfade dipped ~5 dB in the middle
dry_of(g) = dry_g + (1 - dry_g) * sqrt(max(0, 1 - g * g));
aligned(g) = g @ ALIGN : si.smooth(ba.tau2pole(0.005));

// ---- process ------------------------------------------------------------------

// out 1/2: the original crossfading into the frozen sound (the original
// never goes through the FFT: no comb filter), out 3/4: the frozen sound
// alone
process(inl, inr) = ol, or, fl, fr
with {
    // at 4096 points the library runs channel 0 only (two don't fit the
    // fast memory): it gets both inputs, channel 1 returns its output
    in0 = select2(SIZE > 2048, inl, (inl + inr) * 0.5);
    xl  = spectral(0, FREEZE, SIZE, in0, a, step, drift, smear_s, scatter, scatter_s, tilt, morph, layer, grain, ratio, smear_amt, purity, lock);
    xr  = spectral(1, FREEZE, SIZE, inr, a, step, drift, smear_s, scatter, scatter_s, tilt, morph, layer, grain, ratio, smear_amt, purity, lock);
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
