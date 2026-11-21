// SPDX-License-Identifier: GPL-3.0-or-later
declare name "pollen";
declare description "Live granular sampler: grains from a looping live memory (freeze holds it), position, size, pitch, density, spray, blur reverb; free or synced to the transport";
declare author "Arev Imer";
declare version "0.3";
declare block_size "128";       // audio block (system setting block size: auto): 1.33 ms at 96 kHz; spreads the peaks of fast grain reads (64 peaked at 95 % with budget 24)
declare sample_rate "96000";   // system setting sample rate: auto (FM, crush and grains alias less)
declare audio_outputs "1,2,7,8"; // 3-6 are trigger/CV outputs (left exact)
declare mem16 "2";               // the live memory: 2 channels of 16 bit samples (nemesis_mem16.h)
declare license "GPL-3.0-or-later";

// Controls, modes and background: docs/pollen.md

import("stdfaust.lib");
import("nemesis.lib");

N    = 8;        // grain voices
MAXL = 2^20;     // live memory per channel (PSRAM, = NEMESIS_MEM16_LENGTH): 10.9 s at 96 kHz (2^19 cut 8/10 s to 5.5 s there)
BUDGET = 28;     // grains read at most this many samples per sample together (see grain_bank)

// The live memory is 16 bit (half the PSRAM bandwidth of floats: grains
// pitched up at 96 kHz overloaded the module, 2026-11-17), written and read
// through the library's inline functions; sample t goes to t mod MAXL as in
// a FAUST delay line, so the library can save a frozen loop.
mem_put = ffunction(float nm16_put(int, int, float), "nemesis_mem16.h", "");
mem_get = ffunction(float nm16_get(int, float, float), "nemesis_mem16.h", "");
widx    = ((+(1) : %(MAXL)) ~ _) - 1;

// ---- controls (curves from the raw sliders: control rate) ------------------

pos_knob   = hslider("position [hw:CV_X]", 1, 0, 1, 0.001);
size_knob  = hslider("size [hw:CV_Y]", 0.5, 0, 1, 0.001);
pitch_knob = hslider("pitch [hw:CV_Z]", 0.5, 0, 1, 0.001);
dens_knob  = hslider("density [rev1:CV_A] [rev2:POT_A] [cv:CV_A]", 0.5, 0, 1, 0.001);
spray_knob = hslider("spray [rev1:CV_B] [rev2:POT_B] [cv:CV_B]", 0, 0, 1, 0.001);
blur_knob  = hslider("blur [rev1:CV_C] [rev2:POT_C] [cv:CV_C]", 0, 0, 1, 0.001);
mix_knob   = hslider("mix [rev1:CV_D] [rev2:POT_D] [cv:CV_D]", 0.5, 0, 1, 0.01);

freeze_raw = hslider("freeze [loop:freeze] [style:menu{'off':0;'on':1}]", 0, 0, 1, 1) > 0.5;
mem_sel    = hslider("memory [ifnot:sync] [style:menu{'2 s':0;'4 s':1;'8 s':2;'10 s':3}]", 1, 0, 3.999, 1) : int;
mem_bars   = hslider("memory bars [name:memory] [if:sync] [style:menu{'1 bar':0;'2 bars':1;'4 bars':2;'8 bars':3}]", 1, 0, 3.999, 1) : int;
// looper: the frozen memory played directly (its previous pass), input
// added into the frozen loop, and how much of the loop each pass keeps
loop_out_g = hslider("loop out", 0, 0, 1, 0.01) : si.smoo;
overdub    = hslider("overdub", 0, 0, 1, 0.01) : si.smoo;
loop_keep  = hslider("loop feedback", 1, 0, 1, 0.01) : si.smoo;
sync       = hslider("sync [style:menu{'off':0;'on':1}]", 0, 0, 1, 1) > 0.5;
scan       = hslider("scan", 0, -1, 1, 0.01);
rev_prob   = hslider("reverse [rev1:CV_E]", 0, 0, 1, 0.01);               // share of reversed grains
pitch_rand = hslider("pitch spray [unit:st]", 0, 0, 12, 0.1);
quant      = hslider("quantize [style:menu{'off':0;'semitones':1;'octaves':2}]", 0, 0, 2.999, 1) : int;
shape      = hslider("shape [style:menu{'bell':0;'tukey':1;'decay':2;'ramp':3;'square':4;'triangle':5}]", 0, 0, 5.999, 1) : int;
pan_spread = hslider("pan spread", 0.5, 0, 1, 0.01);
tone       = hslider("tone [rev1:CV_F]", 0.5, 0, 1, 0.01);
blur_in    = hslider("blur input", 0, 0, 1, 0.01) : si.smoo;   // input into the blur, also when mix is dry

// ---- live memory ------------------------------------------------------------

// length in samples; held while frozen (a new length would cut the loop).
// A frozen loop is saved on the SD card and loaded when the program starts
// frozen (nemesis_loop.h): the library writes its length to "restored",
// which counts until freeze is turned off.
// memory: seconds, or with sync whole bars at the transport's tempo (halved
// while they don't fit: 8 bars at 120 BPM are 16 s, the memory 10.9 s at
// 96 kHz), so a frozen loop is a whole number of bars
bar        = nm_samples(96);
fit_bars(b) = select2(b * bar > MAXL - 8, b, max(1, b / 2));
mem_bars_n = 2 ^ mem_bars : fit_bars : fit_bars : fit_bars;
mem_len    = select2(sync, ba.selectn(4, mem_sel, 2, 4, 8, 10) * ma.SR, mem_bars_n * bar) : min(MAXL - 8) : int;
// with sync and a running transport, freeze (on and off) takes effect on
// the next bar line: the loop holds the bars before it
freeze     = ba.sAndH(select2(sync & (nm_running > 0), 1, nm_grid(96)), freeze_raw);
restored   = hslider("loop length [loop:restored]", 0, 0, MAXL, 1) : int;
first      = 1 - 1';                                   // 1 in the first sample
L          = select2(restored > 0, ba.sAndH((1 - freeze) | first, mem_len), restored);
// 0 recording .. 1 frozen, already there in the first sample (a restored
// loop must not fade in the input)
frozen     = freeze : si.smooth(select2(first, ba.tau2pole(0.05), 0));
// w(t) = in(t) while recording, w(t - L) while frozen: a loop of L samples
// recording: the input; frozen: the loop's previous pass (times feedback)
// plus the input times overdub
// (soft limited: with feedback 1 every overdub adds to the loop, which grew
// past full scale and the 16 bit memory clipped it hard, recorded for good)
live(ch, x) = mem_put(ch, widx, x * (1 - frozen) + (x * overdub + loop_prev(ch) * loop_keep) * frozen : nm_softclip(1.0));
loop_prev(ch) = mem_get(ch, float(widx) - L, 0);
// the loop as it plays (only while frozen)
loop_play(ch) = loop_prev(ch) * frozen * loop_out_g;

// ---- grain parameters (one shared signal, each voice holds it at its start)

// 20 ms .. the memory length (2/4/8/10 s), exponential; then limited so the
// grain fits in the memory with its pitch (S below: at unison half of it).
// From the memory setting (block rate), not L: a pow of L (a held signal)
// would run on every sample. Was 20 ms .. 2 s.
S_raw      = ma.SR * 0.02 * pow(mem_len / (ma.SR * 0.02), size_knob);
density    = 2 ^ (dens_knob * 5 - 2) : min(N - 1);    // overlapping grains
rate_free  = density * ma.SR / S_raw;                 // grains per second

sync_on    = sync & (nm_running > 0);
// sync: grains per note value, 1 bar .. 1/128 (fractions of a tick work:
// the grid compares the continuous position; 1/32 was too sparse at the top)
ticks      = ba.selectn(8, int(dens_knob * 7.999), 96, 48, 24, 12, 6, 3, 1.5, 0.75);
interval   = nm_samples(ticks);

ph         = (+(rate_free / ma.SR) : ma.frac) ~ _;
trig       = select2(sync_on, ph < ph', nm_grid(ticks));

// independent random values per sample (white noise and its last samples)
rn(k)      = no.noise : de.delay(8, k);

semis_raw  = (pitch_knob - 0.5) * 48 + pitch_rand * rn(1);
semis      = ba.selectn(3, quant, semis_raw, floor(semis_raw + 0.5), 12 * floor(semis_raw / 12 + 0.5));
ratio      = 2 ^ (semis / 12);
reversed   = abs(rn(2)) < rev_prob;
pan        = rn(3) * pan_spread;

// size: the sync grid limits it so at most N - 1 grains overlap; a grain
// must fit in the memory with its pitch
S          = select2(sync_on, S_raw, min(S_raw, (N - 1) * interval))
             : min((L - 16) / (1 + ratio)) : max(64);
overlap    = select2(sync_on, density, S / interval) : max(1);

// start: the age of the audio, behind the newest sample while recording,
// behind the freeze point while frozen (the loop's seam: a grain must not
// read across it). Position, scan (moves the start: 1 forward at the
// original speed) and spray, wrapped to the memory, then kept far enough
// from both ends that the whole grain reads valid audio. The delay to read
// is the age plus the time since the freeze (the loop repeats every L).
wrapL(x)   = x - L * floor(x / L);
drift      = (+(-scan) : wrapL) ~ _;
since_frz  = (+(1) : *(freeze) : wrapL) ~ _;
spray_off  = spray_knob * spray_knob * (rn(4) * 0.5 + 0.5) * L;
age        = wrapL((1 - pos_knob) * L + drift + spray_off);
// recording: forward grains may start at the newest sample unless pitched
// up; frozen: a grain moves towards the seam by ratio * S
age_lo     = select2(freeze, select2(reversed, 2 + S * max(0, ratio - 1), 2 + ratio * S), 2 + ratio * S);
// and no further back than recorded since the start (a restored loop is
// complete): a grain across the edge of the still empty memory set fresh
// audio against silence, crackles while the memory first filled
recorded   = (+(1) : min(MAXL)) ~ _;
reach      = select2(restored > 0, recorded, MAXL);
age_hi     = select2(reversed, L - 4 - S * max(0, 1 - ratio), L - 4 - S) : min(reach - 4 - S * max(1, 2 - ratio));
d_start    = wrapL((age : max(age_lo) : min(age_hi)) + since_frz);


// ---- grain windows (polynomials: no sin/cos per voice) ----------------------

window(p) = ba.selectn(6, shape, bell, tukey, decay, ramp, square, tri)
with {
    bell   = 16 * p * p * (1 - p) * (1 - p);
    e      = min(1, min(p, 1 - p) / 0.2);
    tukey  = e * e * (3 - 2 * e);
    decay  = min(1, p / 0.02) * (1 - p) * (1 - p);
    ramp   = p * p * min(1, (1 - p) / 0.05);
    square = min(1, min(p, 1 - p) / 0.02);
    tri    = 1 - abs(2 * p - 1);
};

// ---- a grain voice: left, right, envelope -----------------------------------

// a voice reads the memory at the delay d behind the newest sample (wl/wr:
// the samples just written, so the read comes after the write); its cost is
// its read speed while it plays (at least 0.5: slow grains still read)
voice(i, tg, vno, wl, wr) = mem_get(0, float(widx) - d, wl) * env * gl, mem_get(1, float(widx) - d, wr) * env * gr, env, cost
with {
    tr   = tg & (vno == i);
    hold = ba.sAndH(tr);
    t    = (+(1) : *(1 - tr) : min(10000000)) ~ _;   // samples since its start
    Sh   = hold(S);
    rh   = hold(ratio);
    revh = hold(reversed);
    dh   = hold(d_start);
    ph_  = hold(pan);
    ever = (max(tr)) ~ _;
    p    = float(t) / Sh;
    env  = window(min(p, 1)) * (p < 1) * ever;
    // forward: start + ratio * t; reversed: from the end backwards. The
    // position stops at the grain's end: an idle voice reads the same place
    // (in the cache) instead of running through PSRAM at grain speed (at +24
    // st that alone overloaded 96 kHz, 2026-11-17)
    te   = min(t, Sh);
    d    = select2(revh, dh + te * (1 - rh), dh - rh * Sh + te * (1 + rh)) : max(0) : min(MAXL - 4);
    gl   = min(1, 1 - ph_);
    gr   = min(1, 1 + ph_);
    cost = (p < 1) * ever * max(0.5, rh);
};

// the voices with a budget: a grain starts (round robin) only while the
// playing grains' read speeds plus its own stay within BUDGET (or none
// plays), so long, dense grains pitched up get sparser instead of
// overloading the module (size 1, density 1, +24 st did); out: left,
// right, envelope sum, the grain triggers that started
grain_bank(ml, mr) = bank ~ (!, !, !, _, !) : _, _, _, !, _
with {
    bank(load) = (par(i, N, voice(i, tg, vno, ml, mr)) :> _, _, _, _), tg
    with {
        tg  = trig & ((load + max(0.5, ratio) <= BUDGET) | (load == 0));
        vno = (+(tg) : %(N)) ~ _;
    };
};

// ---- tone and blur -------------------------------------------------------------

tt      = tone * 2 - 1;
// state variable filters (trapezoidal, as Butterworth with Q 1/sqrt 2): the
// biquads of fi.lowpass/highpass burst up to 40x when the cutoff jumps (a
// turn of tone; tests/programs found it), the SVF stays smooth; the
// cutoffs from the raw knob, so no tan/pow per sample
tone_f  = fi.svf.lp(20000 * 0.01 ^ max(0, -tt), 0.7071) : fi.svf.hp(20 * 100 ^ max(0, tt), 0.7071);

allpass_fixed(n, coeff) = (+ <: de.delay(n, n - 1), *(coeff)) ~ *(-coeff) : mem, _ : +;

// 4 delay lines with a Hadamard matrix: send and decay (0.3 .. 12 s) on one knob
blur_rt   = 0.3 * 40 ^ blur_knob;
blur_g    = 10 ^ (-3 * 0.081 / blur_rt);              // 81 ms: mean loop length
blur_send = sqrt(blur_knob) : si.smoo;
blur(l, r) = (inject : par(i, 4, de.delay(8192, ba.take(i + 1, (2903, 3533, 4177, 4889)) - 1) : damp))
             ~ (mixm : par(i, 4, *(blur_g)))
           : outs
with {
    li     = l * blur_send : allpass_fixed(337, 0.6);
    ri     = r * blur_send : allpass_fixed(557, 0.6);
    inject = +(li), +(ri), +(-li), +(-ri);
    damp   = _ * 0.7 : + ~ *(0.3);
    mixm(a, b, c, d) = (a + b + c + d) * 0.5, (a - b + c - d) * 0.5, (a + b - c - d) * 0.5, (a - b - c + d) * 0.5;
    outs(a, b, c, d) = (a + c) * 0.5, (b + d) * 0.5;
};

// equal power mix, gains from the raw knob
wet_g = sin(mix_knob * ma.PI / 2) : si.smoo;
dry_g = cos(mix_knob * ma.PI / 2) : si.smoo;

// ---- process ------------------------------------------------------------------

// the blur comes after the mix: the grains go in at their wet level, the
// input at "blur input", so the input can be blurred with mix fully dry
process(inl, inr) = (inl * dry_g + (gl + lpl) * wet_g + bl, inr * dry_g + (gr + lpr) * wet_g + br : nm_limiter2(0.9)),
                    pulse, env_cv, rnd_cv, pitch_cv, wl, wr
with {
    ml      = live(0, inl);
    mr      = live(1, inr);
    grains  = grain_bank(ml, mr);
    gain    = 1.5 / sqrt(overlap) : si.smoo;
    gl      = grains : _, !, !, ! : *(gain) : tone_f;
    gr      = grains : !, _, !, ! : *(gain) : tone_f;
    env_sum = grains : !, !, _, !;
    tg      = grains : !, !, !, _;
    bl      = blur(gl * wet_g + inl * blur_in, gr * wet_g + inr * blur_in) : _, !;
    br      = blur(gl * wet_g + inl * blur_in, gr * wet_g + inr * blur_in) : !, _;
    lpl     = loop_play(0);   // the loop: part of the wet signal (grains + level * loop), follows mix
    lpr     = loop_play(1);
    wl      = gl + bl + lpl;  // out 7/8: grains + blur (its grain part follows mix) + loop
    wr      = gr + br + lpr;
    since   = (+(1) : *(1 - tg) : min(10000000)) ~ _;
    pulse   = since < 0.005 * ma.SR;
    env_cv  = env_sum / max(1, 0.6 * overlap) : min(1);
    rnd_cv  = ba.sAndH(tg, rn(5) * 0.5 + 0.5);
    pitch_cv = ba.sAndH(tg, semis) / 12 / 5           // 1 V/oct, 1.0 = 5 V
             : attach(_, L * freeze : hbargraph("loop [loop:length]", 0, MAXL));
};
