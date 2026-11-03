declare name "breaker";
declare description "Beat slice repeat and reverse for breakbeats, bit crush, comb filter and drive; MIDI clock or internal tempo";
declare author "Arev Imer";
declare version "0.1";
declare options "[midi:on]";

// Stereo in 1/2, out 1/2. Out 3: trigger on every slice, out 4: on every beat.
// Display: clock source (MIDI or int) and tempo; LED 1 flashes on the beat,
// LED 2 lights while a slice repeats.
//
// The slice grid follows the MIDI clock while one is running (MIDI Start
// resets it to the downbeat), else the tempo setting. At every slice
// boundary the program decides whether that slice
//   repeats: the slice is recorded once and then looped until repeat ends;
//            shortening the slice while repeating rolls the start of it
//   reverses: plays the previous slice backwards (a repeated one too)
// Gates force it, the chance knobs roll the dice per slice.
//
// hardware controls (rev2)
// slice length, repeat chance, reverse chance   X Y Z (knob + CV)
// repeat gate, reverse gate                     CV A, CV B (> 4 V, or 2.5 V with the 0-5 V range)
// crush (+ CV C), drive                         POT A, POT B
// comb frequency (+ CV D), comb feedback        POT C, POT D
// tempo, dry/wet mix                            settings (turn the encoder)

import("stdfaust.lib");

N = 2^19;                  // slice buffer per channel, 10.9 s at 48 kHz (PSRAM)
FADE = 64;                 // fade in/out at slice edges against clicks, samples

// ---- controls -------------------------------------------------------------

division    = hslider("slice [hw:CV_X] [style:menu{'1/2':0;'1/4':1;'1/8':2;'1/8T':3;'1/16':4;'1/16T':5;'1/32':6;'1/32T':7}]", 4, 0, 7.999, 1) : int;
rep_chance  = hslider("repeat [hw:CV_Y]", 0, 0, 1, 0.01);
rev_chance  = hslider("reverse [hw:CV_Z]", 0, 0, 1, 0.01);
rep_gate    = checkbox("repeat gate [hw:CV_A]");
rev_gate    = checkbox("reverse gate [hw:CV_B]");
crush       = hslider("crush [hw:POT_A] [cv:CV_C]", 0, 0, 1, 0.001) : si.smoo;
drive       = hslider("drive [hw:POT_B]", 0, 0, 1, 0.001) : si.smoo;
comb_tune   = hslider("comb [hw:POT_C] [cv:CV_D]", 0.5, 0, 1, 0.001) : si.smoo;
comb_fb     = hslider("feedback [hw:POT_D]", 0, 0, 1, 0.001) : si.smoo;
tempo       = hslider("tempo", 120, 40, 240, 1);
mix         = hslider("mix", 1, 0, 1, 0.01) : si.smoo;
midi_clock  = checkbox("clock [midi:clock] [hidden:1]");  // toggles on every MIDI clock
midi_run    = checkbox("run [midi:start] [midi:stop] [hidden:1]");

// ---- clock ----------------------------------------------------------------

// samples since the last reset (0 on the reset sample)
counter(reset) = (+(1) : *(1 - reset)) ~ _;

// MIDI clock: 24 ticks per quarter, period measured and averaged
midi_tick   = midi_clock != midi_clock';
since_midi  = counter(midi_tick);
interval    = since_midi' + 1;
valid       = midi_tick & (interval < ma.SR / 4);
midi_period = loop ~ _
with {
    loop(p) = select2(valid, p, select2(p == 0, p + 0.2 * (interval - p), interval));
};
midi_active = (since_midi < ma.SR / 4) & (midi_period > 0);

// internal clock from the tempo setting
int_phase   = (+(24 * tempo / 60 / ma.SR) : ma.frac) ~ _;
int_tick    = int_phase < int_phase';

tick        = select2(midi_active, int_tick, midi_tick);
tick_period = select2(midi_active, ma.SR * 60 / tempo / 24, midi_period);

// ticks since MIDI Start, the start tick counts as the first
start       = midi_run > midi_run';
tick_count  = loop ~ _
with {
    loop(c) = select2(start, c + tick, tick);
};

ticks_per_slice = ba.selectn(8, division, 48, 24, 12, 8, 6, 4, 3, 2);
boundary    = tick & (((tick_count - 1) % ticks_per_slice) == 0);
beat        = tick & (((tick_count - 1) % 24) == 0);

L  = int(max(FADE * 2, min(N / 2 - 1, tick_period * ticks_per_slice)));  // slice length
i  = counter(boundary);              // position in the slice
ic = min(i, L - 1);

// ---- repeat and reverse ---------------------------------------------------

dice(seed)  = no.noises(2, seed) : abs;
rep_on      = ba.sAndH(boundary, (rep_gate > 0.5) | (dice(0) < rep_chance));
rev_on      = ba.sAndH(boundary, (rev_gate > 0.5) | (dice(1) < rev_chance));

rep_start   = rep_on & (1 - rep_on');
rep_age     = counter(rep_start);
frozen      = rep_on & (rep_age > i);   // the first pass is recorded, then looped

w           = (+(1 - frozen) : %(N)) ~ _;   // write position, stops while looping
w0          = ba.sAndH(rep_start, w);      // start of the repeated slice

// looped slice forwards or backwards, otherwise the previous slice backwards
read_pos    = select2(frozen, w - 2 * ic - 1, select2(rev_on, w0 + ic, w0 + L - 1 - ic));
read_index  = (read_pos + N) % N;
use_buffer  = (frozen | rev_on) : si.smooth(ba.tau2pole(0.002));
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

// feedback comb, 30 Hz .. 1.9 kHz
comb(x)     = x : (+ ~ (de.fdelay(4096, delay - 1) : *(fb))) : *(1 - 0.75 * fb)
with {
    delay = ma.SR / (30 * 2 ^ (comb_tune * 6));
    fb = comb_fb * 0.95;
};

drive_stage(x) = x * (1 - drive) + drive * ma.tanh(x * (1 + 15 * drive));

effect(x)   = x : slicer : crusher : comb : drive_stage;
channel(x)  = x * (1 - mix) + effect(x) * mix : max(-1.2) : min(1.2);

pulse(t)    = counter(t) < ma.SR * 0.005;   // 5 ms trigger, 5 V

// ---- display and LEDs -----------------------------------------------------

// home screen: clock source and tempo; LED 1 flashes on the beat, LED 2 while repeating
sync_bar    = midi_active : vbargraph("sync [status:1] [style:menu{'int':0;'MIDI':1}]", 0, 1);
bpm_bar     = ma.SR * 60 / (24 * tick_period) : *(10) : rint : /(10) : vbargraph("bpm [status:1]", 0, 999);
beat_led    = counter(beat) < ma.SR * 0.06 : vbargraph("beat [led:1]", 0, 1);
repeat_led  = frozen : vbargraph("repeating [led:2]", 0, 1);
meters(x)   = x : attach(_, sync_bar) : attach(_, bpm_bar) : attach(_, beat_led) : attach(_, repeat_led);

process     = (channel : meters), channel, pulse(boundary), pulse(beat);
