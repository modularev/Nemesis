// SPDX-License-Identifier: GPL-3.0-or-later
declare name "bitrot";
declare description "Data errors and glitches: a recording buffer that stutters, jumps back, reverses, changes speed, stops like tape and drops out at random on the transport or a free clock; bit flips, crush, stalls and dropouts; repeatable random patterns";
declare author "Arev Imer";
declare version "0.1";
declare block_size "64";            // audio block (system setting block size: auto)
declare audio_outputs "1,2,5,6";    // 3/4 are a trigger and a random CV (left exact)
declare license "GPL-3.0-or-later";

// Controls, modes and background: docs/bitrot.md

import("stdfaust.lib");
import("nemesis.lib");

N    = 2^19;     // buffer per channel (PSRAM), 10.9 s at 48 kHz
STALL = 8192;    // short buffer for stalls (RAM)

// ---- controls (curves from the raw sliders: control rate) ------------------

time_knob = hslider("time [hw:CV_X]", 0.5, 0, 1, 0.001);
chaos     = hslider("chaos [hw:CV_Y]", 0.3, 0, 1, 0.001);
rep_knob  = hslider("repeats [hw:CV_Z]", 0.4, 0, 1, 0.001);
corrupt   = hslider("corrupt [rev1:CV_A] [rev2:POT_A] [cv:CV_A]", 0, 0, 1, 0.001);
depth     = hslider("depth [rev1:CV_B] [rev2:POT_B] [cv:CV_B]", 0.3, 0, 1, 0.001);
bend      = hslider("bend [rev1:CV_C] [rev2:POT_C] [cv:CV_C]", 0.3, 0, 1, 0.001);
mix       = hslider("mix [rev1:CV_D] [rev2:POT_D] [cv:CV_D]", 1, 0, 1, 0.01) : si.smoo;

// settings, numbered: FAUST sorts them by label
sync      = hslider("[01] sync [style:menu{'off':0;'on':1}]", 1, 0, 1, 1) > 0.5;
freeze    = hslider("[02] freeze [style:menu{'off':0;'on':1}]", 0, 0, 1, 1) > 0.5;
pattern   = hslider("[03] pattern [style:menu{'free':0;'2':1;'4':2;'8':3;'16':4;'32':5}]", 0, 0, 5.999, 1) : int;
seed      = hslider("[04] seed", 0, 0, 99, 1) : int;
reroll    = hslider("[05] reroll [style:menu{'off':0;'on':1}]", 0, 0, 1, 1) > 0.5;
w_repeat  = hslider("[10] repeat [section:errors]", 1, 0, 1, 0.01);
w_jump    = hslider("[11] jump [rev1:CV_F]", 0.6, 0, 1, 0.01);
w_reverse = hslider("[12] reverse", 0.5, 0, 1, 0.01);
w_speed   = hslider("[13] speed", 0.4, 0, 1, 0.01);
w_stop    = hslider("[14] tape stop", 0.2, 0, 1, 0.01);
w_gap     = hslider("[15] gap", 0.3, 0, 1, 0.01);
ctype     = hslider("[20] corrupt type [section:sound] [style:menu{'bits':0;'crush':1;'stall':2;'drop':3}]", 0, 0, 3.999, 1) : int;
window    = hslider("[21] window [rev1:CV_E]", 0.1, 0, 1, 0.01);
stereo    = hslider("[22] stereo [style:menu{'linked':0;'split':1}]", 0, 0, 1, 1) > 0.5;

// ---- clock --------------------------------------------------------------------

// samples since the last reset (0 on the reset sample)
counter(reset) = (+(1) : *(1 - reset)) ~ _;

// knob up = shorter: 1 bar .. 1/32
div       = int(time_knob * 7.999);
ticks     = ba.selectn(8, div, 96, 48, 24, 12, 8, 6, 4, 3);
L_free    = 2.0 * pow(0.01, time_knob) * ma.SR;           // 2 s .. 20 ms
L_raw     = select2(sync, L_free, nm_samples(ticks));
L         = int(max(256, min(N / 4, L_raw)));               // event length, samples

use_grid  = sync & (nm_running > 0);
free_clk  = (+(1) <: select2(_ >= L, _, 0)) ~ _;
boundary  = select2(use_grid, free_clk == 0, nm_grid(ticks));
i         = counter(boundary);                              // position in the event
ic        = min(i, L - 1);

// ---- dice -------------------------------------------------------------------

// integer hash (PCG style), 0 .. 1
hash(n)   = float((h2 xor (h2 >> 13)) & 65535) / 65536.0
with {
    h1 = n * 747796405 + 1442695041;
    h2 = (h1 xor (h1 >> 15)) * 1274126177;
};

events    = (+(boundary) ~ _);                              // free event count
event_no  = select2(use_grid, events, int(nm_grid_index(ticks)));
plen      = ba.selectn(6, pattern, 1, 2, 4, 8, 16, 32);
key       = select2(pattern > 0, event_no, event_no % plen);
rerolls   = (+(reroll & (1 - reroll'))) ~ _;
roll(salt, j) = hash(key * 64 + j + (seed + rerolls * 101) * 7919 + salt * 104729);

// ---- the glitch head ----------------------------------------------------------

// error types 0 repeat, 1 jump, 2 reverse, 3 speed, 4 tape stop, 5 gap, 6 none
c1 = w_repeat; c2 = c1 + w_jump; c3 = c2 + w_reverse; c4 = c3 + w_speed;
c5 = c4 + w_stop; c6 = c5 + w_gap;

kind(salt) = select2(happens, 6, t)
with {
    happens = (roll(salt, 0) < chaos) & (c6 > 0);
    r = roll(salt, 1) * c6;
    t = (r >= c1) + (r >= c2) + (r >= c3) + (r >= c4) + (r >= c5);
};

// one delay line per channel: the buffer. Every error reads it at a delay d
// (samples back from now) that follows from where the playback started at
// the event and how fast it moves. Frozen, it recirculates at the event
// length held at the moment of freezing, so its output is that loop.
head(salt, x) = wet, (t < 6)
with {
    t       = kind(salt);
    held    = ba.sAndH(1 - freeze, L);
    frozen_len = select2(held < 2, held, L);   // frozen from the start: the event length
    s       = (select2(freeze, x, _)) ~ (@(max(0, min(N - 2, frozen_len - 1))));

    // repeat: the first S samples of the event play live and then loop
    reps    = 2 ^ int(rep_knob * 5.999);
    S       = max(64, int(L / reps));
    j       = i % S;
    d_rep   = float(i - j);

    // jump: a whole number of events back, up to depth of the buffer
    max_back = max(1, int(depth * float(N - 3 * L - 8) / float(L)));
    d_jump  = float(L * (1 + int(roll(salt, 3) * max_back)));

    // reverse: the past backwards
    d_rev   = float(2 * i + 2);

    // speed: rates from gentle to wild, bend sets how many may come up
    reach   = 1 + int(bend * 5.999);
    rate    = ba.selectn(6, int(roll(salt, 4) * reach), 0.5, -1.0, 2.0, -0.5, -2.0, 0.25);
    d_speed = (1 - rate) * i + 2 + max(0, rate - 1) * L;

    // tape stop: the rate falls from 1 to 0 over the event
    d_stop  = float(i) * float(i) / (2.0 * L) + 2;

    d       = ba.selectn(5, min(t, 4), d_rep, d_jump, d_rev, d_speed, d_stop);
    d0      = int(floor(d));
    frac    = d - floor(d);
    read(k) = s @ max(0, min(N - 2, k));
    played  = read(d0) * (1 - frac) + read(d0 + 1) * frac;

    // window over the piece: a loop of the repeat, else the event
    seg     = select2(t == 0, L, S);
    pos     = select2(t == 0, ic, j);
    fade    = max(1.0, window * 0.5 * seg);
    win     = min(1.0, float(min(pos, seg - 1 - pos)) / fade);

    use_buf = (t < 5) : si.smooth(ba.tau2pole(0.002));
    gap_len = L * (0.25 + 0.75 * roll(salt, 5));
    gap     = ((t == 5) & (i < gap_len)) : si.smooth(ba.tau2pole(0.0005));

    wet     = (s * (1 - use_buf) + played * win * use_buf) * (1 - gap);
};

// ---- corrupt --------------------------------------------------------------------

// countdown that starts at len when start is true and it is idle
timer(start, len) = (\(c).(select2(c > 0, select2(start, 0, len), c - 1))) ~ _;

corrupt_stage(c, x) = ba.selectn(4, ctype, bits, crush, stall, drop)
with {
    a     = corrupt;
    u(k)  = no.noises(8, c * 4 + k) : abs;

    // 16 bit words, a random bit flipped now and then: more often and
    // higher bits (louder) with the amount
    flip  = u(0) < a * a * 0.02;
    bit   = min(14, int(u(1) * (2 + a * 13)));   // not the sign bit: a full scale jump
    xi    = int(max(-1.0, min(0.99997, x)) * 32768.0);
    bits  = float(xi xor (flip * (1 << bit))) / 32768.0;

    // fewer bits and a lower sample rate
    crush = ba.sAndH(hold, x) : q
    with {
        rate = 1 / (1 + 31 * a * a);
        phase = (+(rate) : ma.frac) ~ _;
        hold = phase <= phase';
        steps = 2 ^ (15 - 13 * a);
        q(v) = floor(v * steps + 0.5) / steps;
    };

    // the signal hangs: a grain of g samples repeats while the timer runs
    st_len = int(ma.SR * (0.02 + 0.1 * u(2)));
    st     = timer(u(3) < a * a * 0.0004, st_len);
    st_on  = st > 0;
    st_new = st_on & (st > st');
    since  = counter(st_new);
    g      = ba.sAndH(st_new, int(64 + u(2) * 960));
    stall  = select2(st_on, x, x @ max(0, min(STALL - 1, since + g - since % g)));

    // short dropouts, 5-80 ms
    dr     = timer(u(1) < a * a * 0.0003, int(ma.SR * (0.005 + 0.075 * u(3))));
    drop   = x * (1 - ((dr > 0) : si.smooth(ba.tau2pole(0.0002))));
};

// ---- channels, outputs ----------------------------------------------------------

salt(c) = select2(stereo, 0, c);

channel(c, x) = out, wet, err
with {
    hd  = head(salt(c), x);
    wet = hd : (_, !) : corrupt_stage(c);
    err = hd : (!, _);
    out = x * (1 - mix) + wet * mix;
};

// trigger: high for 5 ms after an error starts
pulse(t) = (max(0, _ - 1), int(0.005 * ma.SR) : select2(t)) ~ _ : >(0);

err_led(e)    = e : vbargraph("error [led:1]", 0, 1);
freeze_led    = freeze : vbargraph("frozen [led:2]", 0, 1);

process(xl, xr) = ol, or, trig, cv, wl, wr
with {
    l = channel(0, xl);
    r = channel(1, xr);
    ol = l : (_, !, !);
    wl = l : (!, _, !);
    el = l : (!, !, _);
    or = r : (_, !, !);
    wr = r : (!, _, !);
    trig = pulse(boundary & (kind(salt(0)) < 6)) : attach(_, err_led(el)) : attach(_, freeze_led);
    cv = ba.sAndH(boundary, roll(0, 7));
};
