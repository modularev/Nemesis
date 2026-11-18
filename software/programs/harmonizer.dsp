declare name "harmonizer";
declare description "Pitch changer and delay after the Eventide H949 Harmonizer: 1 octave up to 2 octaves down, two splice algorithms, micro pitch change, extended and reverse segments, random delay (ADT), flange, repeat, feedback with EQ; manual, MIDI keyboard or V/Oct";
declare author "Arev Imer";
declare version "0.1";
declare options "[midi:on]";      // keyboard control mode: MIDI notes like the HK940 keyboard
declare block_size "64";          // audio block (system setting block size: auto)
declare audio_outputs "1,2";
declare home "ratio=manual|function";   // home screen: the pitch ratio (set by manual: no popup) and the function

// Controls, modes and background: docs/harmonizer.md

import("stdfaust.lib");

MEM   = 2^15;                      // memory (RAM / PSRAM), 682 ms at 48 kHz
LOOP  = 0.4;                       // the H949's memory: 400 ms (repeat loop, extended, reverse)

// ---- controls (curves from the raw sliders: control rate) ------------------

manual    = hslider("manual [hw:CV_X]", 0.6667, 0, 1, 0.001);
main_knob = hslider("main delay [hw:CV_Y]", 0, 0, 1, 0.001);
dly_knob  = hslider("delay only [hw:CV_Z]", 0, 0, 1, 0.001);
fb_main   = hslider("main FB [rev1:CV_A] [rev2:POT_A] [cv:CV_A]", 0, 0, 1, 0.001) : si.smoo;
fb_dly    = hslider("delay FB [rev1:CV_B] [rev2:POT_B] [cv:CV_B]", 0, 0, 1, 0.001) : si.smoo;
eq_low    = hslider("low [rev1:CV_C] [rev2:POT_C] [cv:CV_C]", 0.5, 0, 1, 0.001) : si.smoo;
eq_high   = hslider("high [rev1:CV_D] [rev2:POT_D] [cv:CV_D]", 0.5, 0, 1, 0.001) : si.smoo;

function  = hslider("[01] function [style:menu{'delay':0;'random':1;'flange':2;'reverse':3;'normal':4;'extended':5;'sharp':6;'flat':7}]", 4, 0, 7.999, 1) : int;
alg2      = hslider("[02] algorithm [style:menu{'1':0;'2':1}]", 0, 0, 1, 1) > 0.5;
ctl_mode  = hslider("[03] control [style:menu{'manual':0;'keyboard':1;'cv+man':2}]", 0, 0, 2.999, 1) : int;
quantize  = hslider("[04] quantize [style:menu{'off':0;'semitones':1}]", 0, 0, 1, 1) > 0.5;
glide     = hslider("[05] glide [unit:s]", 0, 0, 2, 0.01);
repeat    = hslider("[06] repeat [style:menu{'off':0;'on':1}]", 0, 0, 1, 1) > 0.5;
input_sel = hslider("[10] input [section:levels] [style:menu{'IN 1':0;'IN 1+2':1}]", 0, 0, 1, 1) > 0.5;
dry       = hslider("[11] dry", 0, 0, 1, 0.01) : si.smoo;
main_lvl  = hslider("[12] main level", 1, 0, 1, 0.01) : si.smoo;
dly_lvl   = hslider("[13] delay only level", 1, 0, 1, 0.01) : si.smoo;
line_out  = hslider("[14] line [style:menu{'in':0;'out':1}]", 0, 0, 1, 1) > 0.5;

note      = hslider("note [mono:note]", 60, 0, 127, 1);        // keyboard: C4 = 1:1
voct      = hslider("v-oct [rev1:CV_E] [volts:1] [setting:0] [unit:V]", 0, -10, 10, 0.001)
          : si.smooth(ba.tau2pole(0.002));

ms(t)     = t * 0.001 * ma.SR;
wrap(x)   = x - floor(x);

// ---- pitch ratio ---------------------------------------------------------------

is_delay   = function == 0;
is_random  = function == 1;
is_flange  = function == 2;
is_reverse = function == 3;
is_extend  = function == 5;
is_sharp   = function == 6;
is_flat    = function == 7;

r_norm    = 0.25 * pow(8, manual);                       // .25 .. 2.00 (2/3 of the way: 1:1)
r_q       = select2(quantize, r_norm, pow(2, floor(12 * log(r_norm) / log(2) + 0.5) / 12));
r_micro   = select2(is_flat, 1 + 0.07 * manual * manual, 1 - 0.07 * manual * manual);
r_manual  = select2(is_sharp | is_flat, r_q, r_micro);
r_key     = pow(2, (note - 60) / 12) : si.smooth(ba.tau2pole(glide + 0.001));
ratio     = ba.selectn(3, ctl_mode, r_manual, r_key, r_manual * pow(2, voct))
          : max(0.25) : min(2.0);

// ---- delays ------------------------------------------------------------------------

step625(v) = int(v * 63.999) * ms(6.25);                  // 0 .. 393.75 ms
offset50  = int(main_knob * 7.999) * ms(50);              // 0 .. 350 ms
segment   = ms(LOOP * 1000) / pow(2, int(main_knob * 3.999));   // 400/200/100/50 ms
dly_only  = max(2, step625(dly_knob));

// splice window and pointer speed: a pointer moving through the window
window    = select2(is_extend | is_reverse, ms(25), segment);
speed     = select2(is_reverse, 1 - ratio, 1 + ratio);     // delay change per sample
phase     = (+(speed / window) : wrap) ~ _;
splice    = abs(phase - phase') > 0.5;                     // the pointer jumped
base      = select2(is_extend | is_reverse, offset50, 0) + 2;

// random: the delay walks towards random targets 0-25 ms at a constant rate
rnd_rate  = 0.1 * pow(100, manual) * 0.001;               // 0.1 .. 10 ms/s, as samples per sample
random_d  = (walk ~ (_, _)) : (_, !)
with {
    walk(d, t) = d + max(0 - rnd_rate, min(rnd_rate, tn - d)), tn
    with {
        target = (no.noise + 1) * 0.5 * ms(25);
        tn = select2(abs(t - d) <= rnd_rate, t, target);
    };
};

// flange: a triangle 0-10 ms
fl_hz     = 0.05 * pow(80, manual);                        // 0.05 .. 4 Hz
fl_phase  = (+(fl_hz / ma.SR) : wrap) ~ _;
flange_d  = (1 - abs(2 * fl_phase - 1)) * ms(10);

// ---- reading the memory ---------------------------------------------------------------

// sig k samples back, linear interpolation; o = 1 inside the feedback loop
// (the ~ already delayed the signal by one sample)
readd(sig, o, d) = sig @ i * (1 - f) + sig @ (i + 1) * f
with {
    d1 = max(0, min(MEM - 4, d - o));
    i  = max(0, min(MEM - 3, int(floor(d1))));
    f  = d1 - floor(d1);
};

// the main output from the memory signal
main_of(sig, o) = ba.selectn(8, function,
        rd(offset_delay),                                         // delay
        twotap(offset50 + 2 + random_d),                          // random
        0.5 * (rd(offset50 + 2) + twotap(offset50 + 2 + flange_d)),  // flange
        pitch, pitch, pitch, pitch, pitch)
with {
    rd(d) = readd(sig, o, d);
    offset_delay = max(2, step625(main_knob));
    // algorithm 1: a second, later copy in random and flange
    twotap(d) = select2(alg2, 0.7 * rd(d) + 0.3 * rd(d + ms(12.5)), rd(d));
    // pointers through the window: one (2) or two crossfaded (1)
    p2 = wrap(phase + 0.5);
    gain(p) = 1 - abs(2 * p - 1);
    head(p) = rd(base + p * window);
    pitch = select2(alg2, head(phase) * gain(phase) + head(p2) * gain(p2), head(phase));
};

dly_of(sig, o) = readd(sig, o, dly_only);

// ---- feedback ----------------------------------------------------------------------

lp1(fc)   = fi.lowpass(1, fc);
hp1(fc)   = fi.highpass(1, fc);

// fitted to the manual's feedback equalization response (page 12)
low_eq(x) = select2(eq_low < 0.5, x + 20 * b * b * lp1(10, x), (1 - c) * x + c * hp1(300, x))
with { c = 1 - 2 * eq_low; b = 2 * eq_low - 1; };
high_eq(x) = select2(eq_high < 0.5, x + 4 * b * fi.highpass(2, 2200, x), (1 - c) * x + c * lp1(2500, x))
with { c = 1 - 2 * eq_high; b = 2 * eq_high - 1; };

feedback(m, d) = (m * fb_main + d * fb_dly) : fi.dcblocker : low_eq : high_eq;

// ---- the memory ------------------------------------------------------------------------

bandlimit = fi.lowpass(4, 15000);
reconstruct = fi.lowpass(2, 15000);
converter = max(-1) : min(1);

// one recursive signal: the feedback block reads the main and delay only
// outputs from the delayed memory signal; the outputs read the same signal
// outside the loop (one delay memory, see CLAUDE.md)
memory(x) = core ~ loop_reads
with {
    loop_reads(sp) = feedback(main_of(sp, 1), dly_of(sp, 1)), sp @ max(0, min(MEM - 3, int(ms(LOOP * 1000)) - 1));
    core(fb, held) = select2(repeat, (x + fb) : bandlimit : converter, held);
};

// ---- process ---------------------------------------------------------------------------

process(in1, in2) = select2(line_out, out_main, x), select2(line_out, out_dly, x)
with {
    x       = in1 + in2 * input_sel;
    s       = memory(x);
    main_w  = main_of(s, 0) : reconstruct;
    dly_w   = dly_of(s, 0) : reconstruct;
    out_main = (main_w * main_lvl + x * dry) : attach(_, ratio_meter) : attach(_, splice_led);
    out_dly  = (dly_w * dly_lvl + x * dry) : attach(_, repeat_led);

    shows_ratio = 1 - (is_delay | is_random | is_flange);
    ratio_meter = select2(shows_ratio, 1, ratio) : vbargraph("ratio [status:1] [digits:3]", 0, 2);
    repeat_led  = repeat : vbargraph("repeating [led:1]", 0, 1);
    splice_led  = (counter(splice & alg2 & shows_ratio) < ma.SR * 0.02) : vbargraph("splice [led:2]", 0, 1);
};

counter(reset) = (+(1) : *(1 - reset)) ~ _;
