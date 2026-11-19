// SPDX-License-Identifier: GPL-3.0-or-later
declare name "primetime";
declare description "Two tap digital delay after the Lexicon Prime Time II (Model 95): one delay line with taps A and B, recirculation with filter and DRC, VCO time base sweep (manual, LFO, envelope), X2 range, infinite repeat, phase invert, input/output mix, clock out";
declare author "Arev Imer";
declare version "0.1";
declare block_size "64";        // audio block (system setting block size: auto)
declare audio_outputs "1-5";    // 6 mod CV, 7/8 clock pulses (left exact)
declare home "A=delay A|B=delay B";   // home screen: both delay times big (their knobs open no popup)
declare license "GPL-3.0-or-later";

// Controls, modes and background: docs/primetime.md

import("stdfaust.lib");
import("nemesis.lib");

N = 2^19;        // the delay memory (PSRAM): 10.9 s at 48 kHz; 7.68 s needed at most

// ---- controls (curves from the raw sliders: control rate) ------------------

sel_a      = hslider("delay A [hw:CV_X]", 0.55, 0, 1, 0.001);
sel_b      = hslider("delay B [hw:CV_Y]", 0, 0, 1, 0.001);
sweep      = hslider("manual sweep [hw:CV_Z]", 0.5, 0, 1, 0.001) * 2 - 1 : si.smoo;   // -1..1, smoothed from 0 = 1X (no glide at the start)
a_fb       = hslider("A-FB [rev1:CV_A] [rev2:POT_A] [cv:CV_A]", 0.3, 0, 1, 0.001) : si.smoo;
b_fb       = hslider("B-FB [rev1:CV_B] [rev2:POT_B] [cv:CV_B]", 0, 0, 1, 0.001) : si.smoo;
depth      = hslider("depth [rev1:CV_C] [rev2:POT_C] [cv:CV_C]", 0, 0, 1, 0.001) : si.smoo;
rate       = hslider("rate [rev1:CV_D] [rev2:POT_D] [cv:CV_D]", 0.3, 0, 1, 0.001);
shape      = hslider("[06] shape [rev1:CV_E]", 0, 0, 1, 0.01) : si.smoo;
master     = hslider("[24] master [rev1:CV_F]", 1, 0, 1, 0.01) : si.smoo;

sync       = hslider("[01] sync [section:delay] [style:menu{'off':0;'on':1}]", 0, 0, 1, 1) > 0.5;
x2         = hslider("[02] range [style:menu{'1X 16 kHz':0;'2X 8 kHz':1}]", 0, 0, 1, 1) > 0.5;
memory     = hslider("[03] memory [style:menu{'640 ms':0;'1.28 s':1;'2.56 s':2}]", 2, 0, 2.999, 1) : int;
// a new delay time: crossfade 25 ms (no pitch) or glide (tape-like pitch bend);
// jumping between the 128 steps crackled
glide      = hslider("[03a] change [style:menu{'crossfade':0;'glide':1}]", 0, 0, 1, 1) > 0.5;
vco        = hslider("[04] vco [section:vco] [style:menu{'xtal':0;'vco':1}]", 1, 0, 1, 1) > 0.5;
square     = hslider("[05] lfo [style:menu{'sine':0;'square':1}]", 0, 0, 1, 1) > 0.5;
repeat     = hslider("[07] repeat [style:menu{'off':0;'on':1}]", 0, 0, 1, 1) > 0.5;
main_in    = hslider("[10] main in [section:input mix]", 1, 0, 1, 0.01) : si.smoo;
aux_in     = hslider("[11] aux in", 0, 0, 1, 0.01) : si.smoo;
filter_hz  = hslider("[12] recirculation filter [unit:Hz] [scale:log]", 20000, 800, 20000, 1);
drc        = hslider("[13] drc [style:menu{'off':0;'on':1}]", 0, 0, 1, 1) > 0.5;
limiter_on = hslider("[14] limiter [style:menu{'off':0;'on':1}]", 0, 0, 1, 1) > 0.5;
main_out   = hslider("[20] main out [section:output mix]", 1, 0, 1, 0.01) : si.smoo;
aux_out    = hslider("[21] aux out", 0, 0, 1, 0.01) : si.smoo;
a_out      = hslider("[22] A out", 0.7, 0, 1, 0.01) : si.smoo;
b_out      = hslider("[23] B out", 0.7, 0, 1, 0.01) : si.smoo;
stereo     = hslider("[25] outputs [style:menu{'mono':0;'A/B':1}]", 1, 0, 1, 1) > 0.5;
inv_main   = hslider("[30] invert main [section:switches] [style:menu{'off':0;'on':1}]", 0, 0, 1, 1) > 0.5;
inv_afb    = hslider("[31] invert A-FB [style:menu{'off':0;'on':1}]", 0, 0, 1, 1) > 0.5;
inv_b      = hslider("[32] invert dly B [style:menu{'off':0;'on':1}]", 0, 0, 1, 1) > 0.5;
bypass     = hslider("[33] bypass [style:menu{'off':0;'on':1}]", 0, 0, 1, 1) > 0.5;
beats      = hslider("[40] beats per loop [section:clock] [style:menu{'1':0;'2':1;'4':2;'8':3;'16':4}]", 2, 0, 4.999, 1) : int;
ppb        = hslider("[41] pulses per beat [style:menu{'1':0;'2':1;'3':2;'4':3;'6':4;'8':5;'12':6;'16':7;'24':8;'32':9;'48':10;'64':11}]", 3, 0, 11.999, 1) : int;

sign(inv)  = 1 - 2 * inv;

// ---- delay select -------------------------------------------------------------

mem_scale  = ba.selectn(3, memory, 1, 2, 4);
range      = 1 + x2;
step(v)    = int(v * 127.999);
set_ms(v)  = rdtable(TAPS, step(v)) * mem_scale * range;   // the tap at 1X time base

// sync: note values, knob up = longer
note_ticks(v) = ba.selectn(15, int(v * 14.999), 3, 4, 6, 8, 9, 12, 16, 18, 24, 32, 36, 48, 72, 96, 192);
set_samples(v) = select2(sync, set_ms(v) * 0.001 * ma.SR, nm_samples(note_ticks(v)));
max_samples = 640 * mem_scale * range * 0.001 * ma.SR;      // the full swing of a delay select

// ---- VCO: the time base ---------------------------------------------------------

// input mix level (main + aux), for the envelope modulation and DRC
level(x)   = x : an.amp_follower_ar(0.002, 0.25);

lfo_hz     = 0.05 * pow(400, rate);                         // 0.05-20 Hz
sine       = os.osc(lfo_hz);
lfo        = select2(square, sine, ma.signum(sine) : si.smooth(ba.tau2pole(0.002)));
// loud = shorter delay
env_mod(e) = 1 - 2 * min(1, e * 4);
mod(e)     = lfo * (1 - shape) + env_mod(e) * shape;         // -1..1, also the mod output
// 0.5..1.5: manual sweep, its range shrinking as depth grows
clock_k(e) = select2(vco, 1, 1 + 0.5 * (sweep * (1 - depth) + mod(e) * depth));

// ---- the delay line ------------------------------------------------------------

bandwidth  = select2(x2, 16000, 8000);
antialias  = fi.lowpass(4, bandwidth);
reconstruct = fi.lowpass(2, bandwidth);
converter  = max(-1) : min(1);                              // the A/D clips at full scale

// input limiter: 4:1 above -4 dB, 18:1 above -1 dB (of full scale)
limiter(x) = x * select2(limiter_on, 1, g)
with {
    e = abs(x) : an.amp_follower_ar(0.001, 0.1) : max(1e-5);
    t1 = 0.63; t2 = 0.89;
    c1 = select2(e > t1, e, t1 + (e - t1) / 4);
    c2 = select2(c1 > t2, c1, t2 + (c1 - t2) / 18);
    g = c2 / e;
};

// DRC: full recirculation below ~-26 dBFS input, less the louder it plays
drc_gain(e) = select2(drc, 1, min(1, 0.05 / max(e, 1e-4)) : si.onePoleSwitching(0.01, 0.3));

// reads a signal d samples back, linear interpolation between samples
readd(d, sig) = sig @ i * (1 - f) + sig @ (i + 1) * f
with { d1 = max(0, min(N - 4, d)); i = max(0, min(N - 3, int(floor(d1)))); f = d1 - floor(d1); };

// recirculation from the taps: A-FB (phase invert) + B-FB, low-pass, DRC
recirc(A, B, e) = (A * a_fb * sign(inv_afb) + B * b_fb) : fi.lowpass(1, filter_hz) : *(drc_gain(e));

// The line: x (the limited input mix) in; taps A, B and the line input out.
// One recursive signal s: the feedback block makes every delayed read of
// it (taps for the recirculation, the repeat loop; ~ adds one sample, so
// they read d - 1), the outputs read the same s outside the loop. (A
// recursion inside another one, or s fed back as a second signal, made
// FAUST build the 2 MB memory two or three times.)
engine(x) = s_line <: tap(A1, A2), tap(B1, B2), _
with {
    e       = level(x);
    k       = clock_k(e);

    // infinite repeat: the memory loops held_len samples (frozen at the
    // moment it engaged) and stops recording
    loop_len = max(64, min(N - 4, max_samples * k));
    held    = ba.sAndH(1 - repeat, loop_len);
    held_len = int(select2(held < 64, held, loop_len) + 0.5);

    // normal: the taps at their delay times the time base; repeat: the
    // playback drifts through the loop at the time base's speed and the
    // taps only offset it
    drift   = (+(1 - 1 / k) : wrap) ~ _
    with { wrap(v) = select2(repeat, 0, v - held_len * floor(v / held_len)); };
    rep(d)  = d + drift - held_len * floor((d + drift) / held_len);
    dd(t)   = max(1, select2(repeat, t * k, rep(t)));

    // two heads per tap, crossfaded by a triangle shared by both taps; a
    // head takes a new step time only while it is silent (crossfade), or
    // both follow the time smoothly (glide). The time base k stays
    // continuous (sweeps, LFO), only the step is held. A steady time: both
    // heads read the same place.
    xf_ph   = (+(1 / (2 * max(16, 0.025 * ma.SR))) : ma.frac) ~ _;
    w       = 1 - abs(2 * xf_ph - 1);                      // head 2's gain
    take_1  = (xf_ph >= 0.5) & (xf_ph' < 0.5);             // head 1 silent
    take_2  = xf_ph < xf_ph';                              // head 2 silent
    step(v, take) = select2(glide, ba.sAndH(take, set_samples(v)),
                            set_samples(v) : si.smooth(ba.tau2pole(0.2)));
    A1      = dd(step(sel_a, take_1));
    A2      = dd(step(sel_a, take_2));
    B1      = dd(step(sel_b, take_1));
    B2      = dd(step(sel_b, take_2));
    heads(sig, o, d1, d2) = readd(d1 - o, sig) * (1 - w) + readd(d2 - o, sig) * w;

    s_line  = core ~ reads;
    reads(sp) = heads(sp, 1, A1, A2), heads(sp, 1, B1, B2), sp @ max(0, min(N - 3, held_len - 1));
    core(ra, rb, rl) = select2(repeat, s0, rl)
    with {
        fb = recirc(ra : reconstruct, rb : reconstruct, e);
        s0 = (x + fb * (1 - bypass)) : antialias : converter;
    };
    tap(d1, d2, sig) = heads(sig, 0, d1, d2) : reconstruct;
};

// ---- clock -------------------------------------------------------------------------

beats_n    = ba.selectn(5, beats, 1, 2, 4, 8, 16);
ppb_n      = ba.selectn(12, ppb, 1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64);
pulse(t)   = (max(0, _ - 1), int(0.005 * ma.SR) : select2(t)) ~ _ : >(0);

// ---- process --------------------------------------------------------------------

// the input mix into the line: main and aux, limited
inmix(main, aux) = (main * main_in + aux * aux_in) : limiter;

process = _, _ <: _, _, (inmix <: _, engine) : mixout;

mixout(main, aux, x, a_sig, b_sig, s_mix) = out_l, out_r, a_sig, b_sig, s_mix, mod_cv, clock_pulse, beat_pulse
with {
    e       = level(x);
    k       = clock_k(e);

    dry     = main * main_out * sign(inv_main) + aux * aux_out;
    wa      = a_sig * a_out * (1 - bypass);
    wb      = b_sig * b_out * sign(inv_b) * (1 - bypass);
    out_l   = (dry + wa + select2(stereo, wb, 0)) * master;
    out_r   = (dry + wb + select2(stereo, wa, 0)) * master;

    mod_cv  = (mod(e) + 1) * 0.5 * vco;

    // clock: one loop = the longer delay (or the repeat loop)
    loop_s  = max(64, select2(repeat, max(set_samples(sel_a), set_samples(sel_b)) * k, max_samples * k));
    phase   = (+(1 / loop_s) : ma.frac) ~ _;
    beat_no = floor(phase * beats_n);
    tick_no = floor(phase * beats_n * ppb_n);
    clock_pulse = pulse(tick_no != tick_no') : attach(_, ms_a) : attach(_, ms_b);
    beat_pulse  = pulse(beat_no != beat_no') : attach(_, beat_led) : attach(_, hold_led);

    beat_led = (counter(beat_no != beat_no') < ma.SR * 0.06) : vbargraph("clock [led:1]", 0, 1);
    hold_led = repeat : vbargraph("hold [led:2]", 0, 1);
    ms_a     = set_samples(sel_a) * k * 1000 / ma.SR : vbargraph("A [status:1] [unit:ms]", 0, 8000);
    ms_b     = set_samples(sel_b) * k * 1000 / ma.SR : vbargraph("B [status:1] [unit:ms]", 0, 8000);
};

counter(reset) = (+(1) : *(1 - reset)) ~ _;

// ---- delay settings ------------------------------------------------------------------

// 128 settings in ms for the standard memory at 1X (640 ms), roughly
// exponential, rounded to prime numbers of 0.1 ms above 1 ms
TAPS = waveform{
    0.0, 0.1, 0.2, 0.3, 0.4, 0.5, 0.7, 0.8, 0.9, 1.1, 1.3, 1.4, 1.7, 1.8, 1.9, 2.3,
    2.4, 2.5, 2.9, 3.0, 3.1, 3.7, 3.8, 4.1, 4.3, 4.7, 4.8, 5.3, 5.4, 5.9, 6.1, 6.7,
    7.1, 7.3, 7.9, 8.3, 8.9, 9.7, 9.8, 10.3, 10.9, 11.3, 12.7, 12.8, 13.7, 13.9, 14.9, 15.7,
    16.7, 17.3, 18.1, 19.3, 19.9, 21.1, 22.3, 23.3, 25.1, 25.7, 27.1, 28.3, 29.3, 31.3, 33.1, 34.7,
    35.9, 37.9, 39.7, 41.9, 43.9, 45.7, 47.9, 50.3, 52.3, 54.7, 57.7, 60.7, 63.1, 66.1, 69.1, 72.7,
    76.1, 79.7, 83.9, 87.7, 91.9, 95.3, 100.9, 105.1, 109.7, 115.1, 120.1, 125.9, 131.9, 138.1, 144.7, 151.1,
    158.3, 165.7, 173.3, 181.1, 190.1, 198.7, 208.1, 217.9, 227.3, 237.7, 247.7, 260.9, 271.9, 285.1, 297.1, 311.9,
    325.9, 341.3, 357.1, 373.3, 390.7, 407.9, 427.1, 446.3, 467.3, 488.9, 511.3, 534.7, 559.1, 585.1, 612.1, 640.0
};
