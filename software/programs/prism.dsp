// SPDX-License-Identifier: GPL-3.0-or-later
declare name "prism";
declare description "Spectral delay: every frequency band through its own delay, skewed from the lows to the highs, with feedback, scatter and freeze";
declare author "Arev Imer";
declare version "0.1";
declare audio_outputs "1-4";
declare block_size "128";
declare spectral "2";            // two channels of spectral processing (nemesis_spectral.h, kernel DELAY)
declare license "GPL-3.0-or-later";

// Controls, modes and background: docs/prism.md

import("stdfaust.lib");
import("nemesis.lib");

spectral = ffunction(float nm_spectral(int, int, int, float, float, float, float, float, float, float, float, float, float, float, float, float, float, float), "nemesis_spectral.h", "");
DELAY    = 1;      // the kernel
SIZE     = 512, 1024, 2048, 4096, 8192 : ba.selectn(5, res_k) : int;   // FFT points (the library reallocates when it changes)

// ---- controls -------------------------------------------------------------------

time_k  = hslider("time [hw:CV_X]", 0.5, 0, 1, 0.001);
skew_k  = hslider("skew [hw:CV_Y]", 0, -100, 100, 1);
fb_k    = hslider("feedback [hw:CV_Z]", 0.4, 0, 1, 0.001);
mix_k   = hslider("mix [hw:POT_A]", 0.5, 0, 1, 0.01);
scatter = hslider("scatter [hw:POT_B] [cv:CV_B]", 0, 0, 1, 0.001);
damp    = hslider("damp [hw:POT_C] [cv:CV_C]", 0, -1, 1, 0.001);
level_k = hslider("level [hw:POT_D] [cv:CV_D]", 0.5, 0, 1, 0.01);
curve   = hslider("[01] curve [style:menu{'linear':0;'octaves':1}]", 1, 0, 1, 1);
res_k   = hslider("[02] resolution [style:menu{'512':0;'1024':1;'2048':2;'4096':3;'8192':4}]", 2, 0, 4, 1) : int;
width   = hslider("[03] width", 1, 0, 1, 0.01) : si.smoo;
tone    = hslider("[04] tone", 0.5, 0, 1, 0.01);
btn     = button("freeze [button:1]");
gate    = hslider("freeze cv [rev2:CV_A] [rev1:CV_E]", 0, 0, 1, 0.001) > 0.5;

// button 1 toggles the freeze, a gate on CV A (rev1: knob/CV E) holds it
toggle(b) = (\(s).(select2(b > b', s, 1 - s))) ~ _;
frozen    = toggle(btn) | gate;

// time 0.02 .. 3 s (from the raw knob: no pow per sample); skew -100 .. 100
time_s    = 0.02 * pow(150, time_k);
skew      = skew_k / 100;
// feedback up to 0.98 (the library holds each band below a full scale sine)
fb        = fb_k * 0.98;

// tone as slush's: state variable filters, cutoffs from the raw setting
tt        = tone * 2 - 1;
tone_f    = fi.svf.lp(20000 * 0.01 ^ max(0, -tt), 0.7071) : fi.svf.hp(20 * 100 ^ max(0, tt), 0.7071);

level     = level_k * 2 : si.smoo;
wet_g     = sin(mix_k * ma.PI / 2) : si.smoo;
dry_g     = cos(mix_k * ma.PI / 2) : si.smoo;

// ---- process ------------------------------------------------------------------

// out 1/2: the input and the delayed spectrum (mix, equal power; the input
// never goes through the FFT), out 3/4: the delayed spectrum alone
process(inl, inr) = ol, or, wl, wr
with {
    xl  = spectral(0, DELAY, SIZE, inl, time_s, skew, fb, scatter, damp, frozen, curve, 0, 0, 0, 0, 0, 0, 0);
    xr  = spectral(1, DELAY, SIZE, inr, time_s, skew, fb, scatter, damp, frozen, curve, 0, 0, 0, 0, 0, 0, 0);
    sl  = xl : tone_f : *(level);
    sr  = xr : tone_f : *(level);
    m   = (sl + sr) * 0.5;
    s   = (sl - sr) * 0.5 * width;
    wl  = m + s;
    wr  = m - s;
    ol  = inl * dry_g + wl * wet_g : nm_softclip(0.9) : attach(_, frozen : vbargraph("frozen [led:1]", 0, 1));
    or  = inr * dry_g + wr * wet_g : nm_softclip(0.9);
};
