declare name "breaker";
declare description "Beat slice repeat and reverse for breakbeats, bit crush, comb filter and drive, plus six drum trigger outputs from a 2D pattern map; MIDI clock or internal tempo";
declare author "Arev Imer";
declare version "0.3";
declare block_size "32";   // audio block (system setting block size: auto)
declare output "clip";     // output setting default: hard clip is part of the sound

// Stereo in 1/2, out 1/2. Outs 3-8: drum triggers (5 V) kick, snare, closed
// hi-hat, open hi-hat, perc 1, perc 2. LED 1 flashes on the beat, LED 2
// lights while a slice repeats.
//
// The slice grid and the drum steps follow the module's transport
// (nemesis.lib): the MIDI clock, a clock or gate on audio IN 8 (clock
// "IN 8", 1/4 to 1/16 or 24 pulses per quarter) or the internal tempo, set
// in the system settings (press and hold the encoder). While the transport
// is stopped the
// input passes through and the triggers are silent.
//
// Slicer: at every slice boundary the program decides whether that slice
//   repeats: the slice is recorded once and then looped until repeat ends;
//            shortening the slice while repeating rolls the start of it
//   reverses: plays the previous slice backwards
// The rev-rep knob rolls the dice per slice: right of the centre for repeats,
// left for reverses, the centre does neither; fully turned = every slice.
// The gates force it.
//
// Comb: centre off; turning either way raises the feedback and the pitch
// (40 Hz .. 1.9 kHz). Left is negative feedback (hollow, an octave lower,
// odd harmonics), right positive (metallic, resonant).
//
// Drums: a 4 x 4 map of one bar patterns (16 sixteenths). Pattern x moves
// through styles (funk, boom bap, break, broken), pattern y from sparse to
// busy. Between the corners the patterns blend: every step has a weight
// 0..9 and fires when the blended weight passes the density threshold, so
// moving a knob adds and drops ghost notes gradually. An open hi-hat chokes
// the closed one on the same step.
//
// hardware controls (rev2)
// slice length, rev-rep, comb                   X Y Z (knob + CV)
// repeat gate, reverse gate                     CV A, CV B (> 4 V, or 2.5 V with the 0-5 V range)
// crush, drive                                  POT A, POT B
// pattern x (+ CV C), pattern y (+ CV D)        POT C, POT D
// settings (turn the encoder): mix, density, swing, trig length

import("stdfaust.lib");
import("nemesis.lib");

N = 2^19;                  // slice buffer per channel, 10.9 s at 48 kHz (PSRAM)
FADE = 64;                 // fade in/out at slice edges against clicks, samples
DEAD = 0.05;               // dead zone around the centre of bipolar knobs

// ---- controls -------------------------------------------------------------

division    = hslider("slice [hw:CV_X] [style:menu{'1/2':0;'1/4':1;'1/8':2;'1/8T':3;'1/16':4;'1/16T':5;'1/32':6;'1/32T':7}]", 4, 0, 7.999, 1) : int;
rev_rep     = hslider("rev-rep [hw:CV_Y]", 0, -1, 1, 0.01);
comb_ctl    = hslider("comb [hw:CV_Z]", 0, -1, 1, 0.001);
rep_gate    = checkbox("repeat gate [hw:CV_A]");
rev_gate    = checkbox("reverse gate [hw:CV_B]");
crush       = hslider("crush [hw:POT_A]", 0, 0, 1, 0.001) : si.smoo;
drive       = hslider("drive [hw:POT_B]", 0, 0, 1, 0.001) : si.smoo;
pat_x       = hslider("pattern x [hw:POT_C] [cv:CV_C]", 0, 0, 1, 0.001);
pat_y       = hslider("pattern y [hw:POT_D] [cv:CV_D]", 0, 0, 1, 0.001);
mix         = hslider("mix", 1, 0, 1, 0.01) : si.smoo;
density     = hslider("density", 0.5, 0, 1, 0.01);
swing       = hslider("swing [unit:%]", 50, 50, 75, 1);
trig_ms     = hslider("trig length [unit:ms]", 5, 1, 50, 1);

// distance from the centre outside the dead zone, 0..1
bipolar(v)  = max(0, abs(v) - DEAD) / (1 - DEAD);

// ---- clock ----------------------------------------------------------------

// samples since the last reset (0 on the reset sample)
counter(reset) = (+(1) : *(1 - reset)) ~ _;

ticks_per_slice = ba.selectn(8, division, 48, 24, 12, 8, 6, 4, 3, 2);
boundary    = nm_grid(ticks_per_slice);
beat        = nm_grid(24);
running     = nm_running > 0;

L  = int(max(FADE * 2, min(N / 2 - 1, nm_samples(ticks_per_slice))));  // slice length
i  = counter(boundary);              // position in the slice
ic = min(i, L - 1);

// ---- repeat and reverse ---------------------------------------------------

rep_chance  = bipolar(rev_rep) * (rev_rep > 0);
rev_chance  = bipolar(rev_rep) * (rev_rep < 0);

dice(seed)  = no.noises(2, seed) : abs;
rep_on      = ba.sAndH(boundary, (rep_gate > 0.5) | (dice(0) < rep_chance));
rev_on      = ba.sAndH(boundary, (rev_gate > 0.5) | (dice(1) < rev_chance));

rep_start   = rep_on & (1 - rep_on');
rep_age     = counter(rep_start);
frozen      = rep_on & (rep_age > i) & running;   // the first pass is recorded, then looped
reversed    = rev_on & running;

w           = (+(1 - frozen) : %(N)) ~ _;   // write position, stops while looping
w0          = ba.sAndH(rep_start, w);      // start of the repeated slice

// looped slice forwards or backwards, otherwise the previous slice backwards
read_pos    = select2(frozen, w - 2 * ic - 1, select2(reversed, w0 + ic, w0 + L - 1 - ic));
read_index  = (read_pos + N) % N;
use_buffer  = (frozen | reversed) : si.smooth(ba.tau2pole(0.002));
window      = min(1.0, float(min(ic, L - 1 - ic)) / FADE);

slicer(x)   = x * (1 - use_buffer) + rwtable(N, 0.0, w, x, read_index) * window * use_buffer;

// ---- effects --------------------------------------------------------------

// bits 16..2 and sample rate down to 1/32
crusher(x)  = ba.sAndH(hold, x) : quantize
with {
    rate = 1 / (1 + 31 * crush * crush);
    phase = (+(rate) : ma.frac) ~ _;
    hold = phase <= phase';
    q = 2 ^ (15 - 14 * crush);
    quantize(s) = floor(s * q + 0.5) / q;
};

// bipolar feedback comb; delay and feedback are computed from the raw knob
// (control rate) and smoothed afterwards
comb(x)     = x : (+ ~ (de.fdelay(4096, delay - 1) : *(fb))) : *(1 - 0.75 * abs(fb))
with {
    amount = bipolar(comb_ctl);
    delay = ma.SR / (40 * 2 ^ (amount * 5.6)) : si.smoo;
    fb = ma.signum(comb_ctl) * sqrt(amount) * 0.95 : si.smoo;
};

drive_stage(x) = x * (1 - drive) + drive * ma.tanh(x * (1 + 15 * drive));

effect(x)   = x : slicer : crusher : comb : drive_stage;
channel(x)  = x * (1 - mix) + effect(x) * mix : max(-1.2) : min(1.2);

// ---- drum triggers ----------------------------------------------------------

// sixteenth steps of the bar; swing delays every second sixteenth
// (50 % straight .. 75 % = a dotted feel)
pair_phase  = fmod(nm_position, 12) / 12;
step        = (int(floor(nm_position / 12)) * 2 + (pair_phase >= swing / 100)) % 16;
step_trig   = running & ((step != step') | (nm_running' == 0));

// bilinear blend of the four patterns around (pattern x, pattern y)
gx          = pat_x * 3;
gy          = pat_y * 3;
cx          = min(2, int(gx));
cy          = min(2, int(gy));
fx          = gx - cx;
fy          = gy - cy;

cell(ix, iy, inst) = rdtable(PATTERNS, ((iy * 4 + ix) * 6 + inst) * 16 + step);
weight(inst) = ((cell(cx, cy, inst) * (1 - fx) + cell(cx + 1, cy, inst) * fx) * (1 - fy)
              + (cell(cx, cy + 1, inst) * (1 - fx) + cell(cx + 1, cy + 1, inst) * fx) * fy) / 9;

// density 0: only the main hits (weight 9), 1: every ghost note
threshold   = 0.95 - 0.9 * density;
fires(inst) = step_trig & (weight(inst) > threshold);

// trigger: high for trig length after it fires
pulse(t)    = (max(0, _ - 1), trig_len : select2(t)) ~ _ : >(0)
with {
    trig_len = trig_ms * ma.SR / 1000;
};

open_hh     = fires(3);
closed_hh   = fires(2) & (1 - open_hh);
drums       = pulse(fires(0)), pulse(fires(1)), pulse(closed_hh), pulse(open_hh),
              pulse(fires(4)), pulse(fires(5));

// ---- display and LEDs -----------------------------------------------------

// LED 1 flashes on the beat, LED 2 while repeating
beat_led    = counter(beat) < ma.SR * 0.06 : vbargraph("beat [led:1]", 0, 1);
repeat_led  = frozen : vbargraph("repeating [led:2]", 0, 1);
meters(x)   = x : attach(_, beat_led) : attach(_, repeat_led);

process     = (channel : meters), channel, drums;

// ---- pattern map --------------------------------------------------------------

// 16 patterns (y rows of 4 x columns) x 6 instruments x 16 steps, weights 0..9:
// 9 main hits, 7..5 secondary, 3 ghost notes
PATTERNS = waveform{
    // x 0 (funk), y 0
    9,0,0,0, 0,0,0,0, 9,0,0,0, 0,0,0,0,   // kick 
    0,0,0,0, 9,0,0,0, 0,0,0,0, 9,0,0,0,   // snare
    9,0,0,0, 9,0,0,0, 9,0,0,0, 9,0,0,0,   // hh   
    0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,   // open 
    0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,   // perc1
    0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,   // perc2
    // x 1 (boom bap), y 0
    9,0,0,0, 0,0,0,0, 0,0,9,0, 0,0,0,0,   // kick 
    0,0,0,0, 9,0,0,0, 0,0,0,0, 9,0,0,0,   // snare
    9,0,0,0, 9,0,0,0, 9,0,0,0, 9,0,0,0,   // hh   
    0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,   // open 
    0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,   // perc1
    0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,   // perc2
    // x 2 (break), y 0
    9,0,0,0, 0,0,0,0, 0,0,9,0, 0,0,0,0,   // kick 
    0,0,0,0, 9,0,0,0, 0,0,0,0, 0,9,0,0,   // snare
    9,0,0,0, 9,0,0,0, 9,0,0,0, 9,0,0,0,   // hh   
    0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,   // open 
    0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,   // perc1
    0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,   // perc2
    // x 3 (broken), y 0
    9,0,0,0, 0,0,0,0, 0,0,9,0, 0,0,0,0,   // kick 
    0,0,0,0, 9,0,0,5, 0,0,0,0, 0,0,9,0,   // snare
    9,0,0,0, 9,0,0,0, 9,0,0,0, 9,0,0,0,   // hh   
    0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,   // open 
    0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,   // perc1
    0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,   // perc2
    // x 0 (funk), y 1
    9,0,0,0, 0,0,0,0, 9,0,3,0, 0,0,0,0,   // kick 
    0,0,0,0, 9,0,0,0, 0,0,0,0, 9,0,0,0,   // snare
    9,0,5,0, 9,0,5,0, 9,0,5,0, 9,0,5,0,   // hh   
    0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,   // open 
    0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,   // perc1
    0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,3,0,   // perc2
    // x 1 (boom bap), y 1
    9,0,0,0, 0,0,0,0, 0,0,9,0, 0,5,0,0,   // kick 
    0,0,0,0, 9,0,0,0, 0,0,0,0, 9,0,0,0,   // snare
    9,0,5,0, 9,0,5,0, 9,0,5,0, 9,0,5,0,   // hh   
    0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,   // open 
    0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,   // perc1
    0,0,0,0, 0,0,0,3, 0,0,0,0, 0,0,0,0,   // perc2
    // x 2 (break), y 1
    9,0,9,0, 0,0,0,0, 0,0,9,0, 0,0,0,0,   // kick 
    0,0,0,0, 9,0,0,5, 0,0,0,0, 9,0,0,5,   // snare
    9,0,5,0, 9,0,5,0, 9,0,5,0, 9,0,5,0,   // hh   
    0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,   // open 
    0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,   // perc1
    0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,   // perc2
    // x 3 (broken), y 1
    9,0,0,0, 0,0,0,0, 0,9,0,9, 0,0,0,0,   // kick 
    0,0,0,0, 9,0,0,5, 0,0,0,0, 0,9,0,0,   // snare
    9,0,5,0, 9,0,5,0, 9,0,5,0, 9,0,5,0,   // hh   
    0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,   // open 
    0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,   // perc1
    0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,   // perc2
    // x 0 (funk), y 2
    9,0,0,0, 0,0,3,0, 9,0,5,0, 0,0,0,0,   // kick 
    0,0,0,0, 9,0,3,0, 0,0,0,0, 9,0,3,0,   // snare
    9,0,5,0, 9,0,5,0, 9,0,5,0, 9,0,3,0,   // hh   
    0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,7,0,   // open 
    0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,   // perc1
    0,0,3,0, 0,0,3,0, 0,0,3,0, 0,0,3,0,   // perc2
    // x 1 (boom bap), y 2
    9,0,0,0, 0,0,0,7, 0,0,9,0, 0,5,0,0,   // kick 
    0,0,0,0, 9,0,0,0, 0,0,0,0, 9,0,0,3,   // snare
    9,0,5,0, 9,0,5,0, 9,0,5,0, 9,0,5,0,   // hh   
    0,0,0,0, 0,0,0,0, 0,0,0,0, 7,0,0,0,   // open 
    0,0,0,0, 7,0,0,0, 0,0,0,0, 7,0,0,0,   // perc1
    0,0,0,3, 0,0,0,0, 0,0,0,3, 0,0,0,0,   // perc2
    // x 2 (break), y 2
    9,0,9,0, 0,0,0,0, 0,0,9,5, 0,0,0,0,   // kick 
    0,0,0,0, 9,0,0,5, 0,0,3,0, 9,0,0,5,   // snare
    9,0,5,0, 9,0,5,0, 9,0,5,0, 9,0,5,0,   // hh   
    0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,7,0,   // open 
    0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,   // perc1
    0,0,0,0, 0,0,3,0, 0,0,0,0, 0,3,0,0,   // perc2
    // x 3 (broken), y 2
    9,0,0,0, 0,0,0,0, 0,9,0,9, 0,0,3,0,   // kick 
    0,0,3,0, 9,0,0,5, 0,0,3,0, 0,9,0,0,   // snare
    9,3,5,3, 9,3,5,3, 9,3,5,3, 9,3,5,3,   // hh   
    0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,9,0,   // open 
    0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,   // perc1
    0,0,0,0, 0,0,0,5, 0,0,0,0, 0,0,5,0,   // perc2
    // x 0 (funk), y 3
    9,0,0,0, 0,0,5,0, 9,0,7,0, 0,0,3,0,   // kick 
    0,0,3,0, 9,0,3,0, 0,3,0,0, 9,0,3,5,   // snare
    9,3,5,3, 9,3,5,3, 9,3,5,3, 9,3,0,0,   // hh   
    0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,9,0,   // open 
    0,0,0,0, 7,0,0,0, 0,0,0,0, 7,0,0,0,   // perc1
    5,3,5,3, 5,3,5,3, 5,3,5,3, 5,3,5,3,   // perc2
    // x 1 (boom bap), y 3
    9,0,3,0, 0,0,0,7, 0,0,9,0, 3,5,0,0,   // kick 
    0,0,0,0, 9,0,0,3, 0,0,0,0, 9,0,0,3,   // snare
    9,3,5,3, 9,3,5,3, 9,3,5,3, 9,3,5,3,   // hh   
    0,0,7,0, 0,0,0,0, 0,0,0,0, 0,0,7,0,   // open 
    0,0,0,0, 7,0,0,0, 0,0,0,0, 7,0,0,5,   // perc1
    0,3,0,0, 0,3,0,0, 0,3,0,0, 0,3,0,0,   // perc2
    // x 2 (break), y 3
    9,0,9,0, 0,0,5,0, 0,0,9,5, 0,0,3,0,   // kick 
    0,0,3,0, 9,0,0,5, 0,0,3,0, 9,0,3,5,   // snare
    9,3,5,3, 9,3,5,3, 9,3,5,3, 9,3,0,0,   // hh   
    0,0,0,0, 0,0,0,7, 0,0,0,0, 0,0,9,0,   // open 
    0,0,0,0, 7,0,0,0, 0,0,0,0, 7,0,0,0,   // perc1
    3,0,0,3, 0,0,3,0, 0,3,0,0, 3,0,0,3,   // perc2
    // x 3 (broken), y 3
    9,0,0,0, 3,0,0,0, 0,9,0,9, 0,0,3,0,   // kick 
    0,0,3,5, 9,0,3,5, 0,0,3,5, 0,9,3,5,   // snare
    9,3,9,3, 9,3,9,3, 9,3,9,3, 9,3,9,3,   // hh   
    0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,9,0,   // open 
    0,0,0,0, 7,0,0,0, 0,0,0,0, 7,0,0,0,   // perc1
    5,0,0,5, 0,0,5,0, 0,5,0,0, 5,0,0,5    // perc2
};
