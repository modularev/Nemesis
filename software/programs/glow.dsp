// SPDX-License-Identifier: GPL-3.0-or-later
declare name "glow";
declare description "Vactrol band-pass and MOSFET drive in a feedback loop: low, high and band pass by width, warmth to fuzz, resonance to self-oscillation, LFOs and resonant CV processing";
declare author "Arev Imer";
declare version "0.1";
declare audio_outputs "1-2";
declare license "GPL-3.0-or-later";

// Controls, modes and background: docs/glow.md

import("stdfaust.lib");
import("nemesis.lib");

// ---- controls -------------------------------------------------------------------

freq_k  = hslider("frequency [hw:CV_X]", 0.5, 0, 1, 0.0001);
width_k = hslider("width [hw:CV_Y]", 0.35, 0, 1, 0.001);
fb_k    = hslider("feedback [hw:CV_Z]", 0.3, 0, 1, 0.001);
drive_k = hslider("drive [hw:POT_A]", 0.2, 0, 1, 0.001);
lag_k   = hslider("vactrol [hw:POT_B] [cv:CV_B]", 0.4, 0, 1, 0.001);
bias_k  = hslider("bias [hw:POT_C] [cv:CV_C]", 0.3, 0, 1, 0.001);
mix_k   = hslider("mix [hw:POT_D] [cv:CV_D]", 1, 0, 1, 0.01);
cv_a    = hslider("cv a [rev1:CV_E] [cv:CV_A] [volts:1] [setting:0] [unit:V]", 0, -10, 10, 0.001);
input   = hslider("[01] input [style:menu{'audio':0;'cv a':1}]", 0, 0, 1, 1);
range   = hslider("[02] range [style:menu{'audio':0;'lfo':1}]", 0, 0, 1, 1);
spread  = hslider("[03] spread [unit:st]", 0, -12, 12, 0.1);
polar   = hslider("[04] feedback polarity [style:menu{'+':0;'-':1}]", 0, 0, 1, 1);
level   = hslider("[05] level [unit:dB]", 0, -24, 12, 0.5) : ba.db2linear : si.smoo;

R = 0.75;   // damping of each filter section (a little over Butterworth: the resonance comes from the loop)

// ---- frequency, in octaves above FMIN -------------------------------------------
//
// One table of the filter coefficient g = tan(pi f / SR) over 21 octaves
// (0.01 Hz .. 21 kHz, clamped at 0.45 SR), read with linear interpolation:
// the edges move every sample (the vactrols), a tan and a pow per sample and
// edge would cost far more (and run through libm)

FMIN  = 0.01;
OCT   = 21;
TN    = 2048;
gtab(o) = t0 + (t1 - t0) * fr
with {
    idx = max(0, min(TN - 1.001, o / OCT * (TN - 1)));
    i0  = int(idx);
    fr  = idx - i0;
    gen = ba.time * (OCT / (TN - 1)) : \(x).(tan(ma.PI * min(FMIN * pow(2, x), 0.45 * ma.SR) / ma.SR));
    t0  = rdtable(TN, gen, i0);
    t1  = rdtable(TN, gen, i0 + 1);
};

// the knob's octave: audio 20 Hz .. 18 kHz, lfo 0.02 .. 100 Hz; CV A adds
// 1 V/oct while it isn't the input (from the raw knob: no log per sample)
lo_a  = log(20 / FMIN) / log(2);
hi_a  = log(18000 / FMIN) / log(2);
lo_l  = log(0.02 / FMIN) / log(2);
hi_l  = log(100 / FMIN) / log(2);
centre = select2(range > 0.5, lo_a + freq_k * (hi_a - lo_a), lo_l + freq_k * (hi_l - lo_l))
       + select2(input > 0.5, cv_a, 0);
// width: each edge 0.08 .. 4.1 octaves from the middle (narrow: a resonant
// band; wide and low: a low pass, wide and high: a high pass)
half  = 0.08 + width_k * width_k * 4;
// a narrow band loses level where its edges overlap: two 2-pole sections
// with damping R at r = 2^half from the middle pass r^4 / ((r^2 - 1)^2 +
// 4 R^2 r^2) there; made up, so the band passes 0 dB in the middle and
// feedback reaches oscillation at the same knob position whatever the width
r2      = pow(2, 2 * half);
makeup  = ((r2 - 1) * (r2 - 1) + 4 * R * R * r2) / (r2 * r2);

// ---- the vactrol ------------------------------------------------------------
//
// An LED lights an LDR: the resistance falls fast when the light rises and
// recovers slowly when it fades (a few ms on, tens of ms to seconds off,
// slower the darker it gets). Each filter edge follows its target octave
// that way: sweeps up snap, sweeps down sag, and the two edges lag apart

t_on  = 0.002 + lag_k * lag_k * 0.06;
t_off = 0.025 + lag_k * lag_k * 1.5;
a_on  = 1 - exp(-1 / (t_on * ma.SR));
vactrol(target) = (\(v).(v + (target - v) * select2(target > v, a_off(v), a_on))) ~ _
with {
    // darker (lower) recovers more slowly: up to 2x at the bottom
    a_off(v) = 1 - exp(-1 / (t_off * ma.SR)) : *(0.5 + 0.5 * min(1, max(0, v / OCT)));
};

// ---- the MOSFET stage ---------------------------------------------------------
//
// Drive up to 40 dB into an asymmetric soft clipper: the positive side
// rounds off at 1, the negative side earlier as the bias rises (even
// harmonics, a rougher fuzz); the bias's own offset is taken off again, so
// silence stays at 0 V (the outputs are DC coupled)

gd    = ba.db2linear(drive_k * 40);
bias  = bias_k * 0.6;
tn    = 1 - 0.65 * bias_k;
th(x) = max(-1, min(1, x * (27 + x * x) / (27 + 9 * x * x)));   // tanh, rational (no libm)
sat(v) = select2(v > 0, tn * th(v / tn), th(v));
// the stage's small-signal gain at the bias, taken off (drive 0 passes at
// unity), and a level compensation that settles at about -11 dB when
// fully saturated (a 2 V sine kept within ~3 dB from warm to fuzz)
slope = (sat(bias + 0.001) - sat(bias - 0.001)) / 0.002;
comp  = 0.28 + 0.72 / (1 + (gd - 1) * 0.6);
mosfet(x) = (sat(gd * x + bias) - sat(bias)) * comp / slope;

// ---- the filter: two TPT state variable sections ------------------------------

tpt(g, x) = loop(x) ~ (_, _) : (!, !, _, _, _)
with {
    loop(xx, s1, s2) = s1n, s2n, lp, bp, hp
    with {
        hp  = (xx - (2 * R + g) * s1 - s2) / (1 + 2 * R * g + g * g);
        v1  = g * hp;
        bp  = v1 + s1;
        s1n = bp + v1;
        v2  = g * bp;
        lp  = v2 + s2;
        s2n = lp + v2;
    };
};
highpass(g) = tpt(g) : (!, !, _);
lowpass(g)  = tpt(g) : (_, !, !);

// ---- one channel ----------------------------------------------------------------

// feedback 0 .. 1.25 (about 0.8 on the knob is the oscillation threshold),
// inverted with polarity -; a -100 dB noise lets an oscillation start
// without input
fbg   = fb_k * 1.25 * select2(polar > 0.5, 1, -1);
seed  = no.noise * 0.00001;

// start: an oscillation grows from the noise by a few percent per period,
// at 0.3 Hz that took minutes. While the feedback is above the threshold and
// the output has been quiet for two periods, a small step goes into the
// loop: the band rings at once and the oscillation takes over
above = abs(fbg) >= 1;
period(o) = 1 / (FMIN * pow(2, o));
kick(o, y) = select2(above & (peak < 0.01), 0, 0.2)
with {
    rel  = exp(-1 / (2 * max(0.05, period(o)) * ma.SR));   // the follower holds two periods
    peak = abs(y) : (\(p, v).(max(v, p * rel))) ~ _;
};

channel(offset, x) = x : (loop ~ (_, _)) : (!, _)
with {
    o    = centre + offset;
    gl   = vactrol(o - half) : gtab;
    gh   = vactrol(o + half) : gtab;
    core = +(seed) : mosfet : highpass(gl) : lowpass(gh) : *(makeup);
    // the loop: its last output (fed back) and the output
    loop(yprev, ydup, xx) = y, y
    with {
        y = xx + yprev * fbg + kick(o, ydup) : core;
    };
};

// ---- process ------------------------------------------------------------------

// the input: audio IN 1/2, or CV A in volts (1.0 = 5 V, read once per block:
// smoothed over 2 ms)
cv_sig = cv_a / 5 : si.smooth(ba.tau2pole(0.002));
src(a) = select2(input > 0.5, a, cv_sig);

wet_g = mix_k : si.smoo;

// out 1/2: dry and processed (mix), out 3/4: the processed signal alone,
// unclipped and DC coupled (CV: envelopes, LFOs)
process(inl, inr) = ol, or, yl * level, yr * level
with {
    yl  = channel(0, src(inl));
    yr  = channel(spread / 12, src(inr));
    ol  = inl * (1 - wet_g) + yl * level * wet_g : nm_softclip(0.9);
    or  = inr * (1 - wet_g) + yr * level * wet_g : nm_softclip(0.9);
};
