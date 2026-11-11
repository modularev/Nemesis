declare name "growler";
declare description "Stereo 4-operator FM bass built around cross modulation, operators played by keys or V/Oct: a depth knob per operator, carrier and operator 4 modulating each other, feedback, 16 ratio sets, ladder filter; MIDI or V/Oct + gate";
declare author "Arev Imer";
declare version "0.7";
declare block_size "32";   // audio block (system setting block size: auto)
declare sample_rate "96000";   // system setting sample rate: auto (FM, crush and grains alias less)
declare options "[midi:on]";
declare audio_outputs "1 2";   // outs 3/4 are CVs (output setting leaves them exact)

// One voice in stereo:
//
//   4 sine operators in one of 6 algorithms (mode), carrier and operator 4
//   modulating each other (cross) --> wavefolder --> 24 dB ladder filter
//      --+--> amp envelope --> out 1 (left), the same detuned --> out 2
//   sub sine (one octave below the carrier, clean, centred) --+
//
// width detunes the left and right networks against each other (0: mono).
//
// Playing the operators (setting keys), each at its pitch times its ratio:
//   spread       the operators are shared by the notes played together: one
//                note all four, two notes 1+2 and 3+4, three notes 1+2 for
//                the lowest and 3, 4 for the others, four notes one each
//                (in order of pitch); lifting keys keeps it, a note played
//                alone starts anew. In twin mode two notes are two complete
//                FM voices (2 -> 1, 4 -> 3), in parallel mode a chord.
//   round robin  a note played alone sets all four operators; notes added
//                while keys are held go to the next operator in turn (2, 3,
//                4, 1 ..) and stay when lifted, until the next note played
//                alone sets all four again
//   held         the first held key is the carrier, operators 2..4 follow it;
//                a second, third, fourth key held at the same time sets
//                operator 2, 3, 4 to its pitch while it is held
// The intervals between the notes become the FM ratios (in parallel mode a
// four note chord). Each operator also has a V/Oct target, v-oct 1..4 (CV
// A-D as assigned in the program settings): v-oct 1 transposes everything,
// v-oct 2..4 offset their operator on top.
//
// cv gate (or any key) opens the envelope. vca off: no envelope, the sound
// keeps going (drone), keys and V/Oct only set the pitches.
//
// The knobs play the modulation network: mod 2/3/4 set how much each
// operator takes part (how hard it modulates, or, where the mode makes it a
// carrier, its level and how hard it is modulated), cross lets the carrier
// and operator 4 modulate each other (both ways, in every mode: from grit
// to chaos), feedback feeds operator 4 and the carrier into themselves
// (towards saws, then noise), ratios steps through 16 sets of ratios for
// operators 2, 3, 4: round and saw-like, harmonic and bright, hollow (odd
// harmonics), then inharmonic and metallic.
//
// Algorithms (setting mode), modulators -> carriers:
//   stack   4 -> 3 -> 2 -> 1        the most aggressive, metallic
//   Y       3 -> 2 -> 1, 4 -> 1     two paths into the carrier
//   twin    2 -> 1, 4 -> 3          two voices, carrier 3 at its ratio
//   fan     4 -> 1, 2, 3            one modulator, three carriers
//   branch  4 -> 3 -> 1, 2 -> 1     a bright path and a direct one
//   three   2, 3, 4 -> 1            three modulators on one carrier, thick
//   parallel 1, 2, 3, 4 all carriers  four tones side by side (a chord from
//                                     four keys or v-oct 1..4); mod 2/3/4
//                                     set their levels, cross and feedback
//                                     still bend carrier and operator 4
//
// The depths follow the knobs only (no envelopes) and drop above ~110 Hz so
// high notes stay clean. A decay envelope, restarted on every new note
// (legato and chords too), opens the filter (env filter, filter decay).
//
// in: none; V/Oct 1..4 and gate on CV A-D as assigned (rev2, V/Oct
//     calibrated; gate > 4 V or 2.5 V with the 0-5 V range), rev1: V/Oct 1
//     on CV E, gate on CV F
// out 1/2: audio left/right
// out 3: filter envelope, 0..5 V         out 4: amp envelope, 0..5 V
//
// hardware controls (rev2)                          rev1
// mod 2, mod 3, mod 4         X Y Z (knob + CV)     CV X Y Z
// V/Oct 1, gate               CV A, CV B (assignable) CV E, CV F
// cross, feedback             POT A, POT B          CV A, B
// ratios, cutoff              POT C, POT D (+ CV C, D)   CV C, D
// settings (turn the encoder): mode, keys, vca, width, drive, resonance,
//   env filter, filter decay, sub, octave, glide, attack, release,
//   velocity, level
// MIDI: notes (spread, round robin or up to 4 held keys), velocity (last key), pitch
//   bend (+-2 semitones)

import("stdfaust.lib");

// ---- controls -------------------------------------------------------------

// curves are computed from the raw sliders (control rate) and smoothed last
depth(x)    = x * sqrt(x) : si.smoo;
mod2_knob   = hslider("mod 2 [hw:CV_X]", 0.4, 0, 1, 0.001);
mod3_knob   = hslider("mod 3 [hw:CV_Y]", 0.3, 0, 1, 0.001);
mod2        = mod2_knob : depth;
mod3        = mod3_knob : depth;
mod4_knob   = hslider("mod 4 [hw:CV_Z]", 0.3, 0, 1, 0.001);
mod4        = mod4_knob : depth;
// CV targets only (not in the settings list): V/Oct per voice in volts, the gate
voct1       = hslider("v-oct 1 [rev1:CV_E] [cv:CV_A] [volts:1] [setting:0] [unit:V]", 0, -10, 10, 0.001);
voct2       = hslider("v-oct 2 [volts:1] [setting:0] [unit:V]", 0, -10, 10, 0.001);
voct3       = hslider("v-oct 3 [volts:1] [setting:0] [unit:V]", 0, -10, 10, 0.001);
voct4       = hslider("v-oct 4 [volts:1] [setting:0] [unit:V]", 0, -10, 10, 0.001);
cv_gate     = checkbox("cv gate [rev1:CV_F] [cv:CV_B] [setting:0]");
// off: no envelope, the sound keeps going (drone)
vca         = hslider("vca [style:menu{'off':0;'on':1}]", 1, 0, 1, 1) > 0.5;
cross       = hslider("cross [hw:POT_A]", 0.2, 0, 1, 0.001) : \(c).(0.8 * c * c) : si.smoo;
feedback    = hslider("feedback [hw:POT_B]", 0.2, 0, 1, 0.001) : \(f).(0.32 * f * f) : si.smoo;
// ratios of operators 2, 3, 4 (the carrier is 1): soft to metallic
ratios_sel  = hslider("ratios [hw:POT_C] [cv:CV_C] [style:menu{'1 1 1':0;'2 1 1':1;'1 2 1':2;'.5 1 1':3;'2 2 1':4;'2 3 1':5;'1 3 2':6;'.5 2 3':7;'3 2 4':8;'1.5 3 2':9;'3 5 7':10;'2 4 7':11;'.5 1.5 5':12;'1.41 2 3.5':13;'2.76 1 5.4':14;'3.14 1.73 7.1':15}]", 5, 0, 15.999, 1) : int;
RATIO_SETS(k) = ba.selectn(16, ratios_sel, k);
// a set changes with a 5 ms glide, no click
glide5      = si.smooth(ba.tau2pole(0.005));
ratio2      = RATIO_SETS((1, 2, 1, 0.5, 2, 2, 1, 0.5, 3, 1.5, 3, 2, 0.5, 1.414, 2.76, 3.14)) : glide5;
ratio3      = RATIO_SETS((1, 1, 2, 1,   2, 3, 3, 2,   2, 3,   5, 4, 1.5, 2,     1,    1.73)) : glide5;
ratio4      = RATIO_SETS((1, 1, 1, 1,   1, 1, 2, 3,   4, 2,   7, 7, 5,   3.5,   5.4,  7.1)) : glide5;
cutoff_n    = hslider("cutoff [hw:POT_D] [cv:CV_D] [scale:log] [unit:Hz]", 4000, 30, 12000, 1)
              : \(c).(log10(c / 20) / 3) : si.smoo;   // ladder's 0..1 = 20 Hz..20 kHz

mode        = hslider("mode [style:menu{'stack':0;'Y':1;'twin':2;'fan':3;'branch':4;'three':5;'parallel':6}]", 1, 0, 6.999, 1) : int;
width       = hslider("width", 0.4, 0, 1, 0.01) : si.smoo;
drive       = hslider("drive", 0.2, 0, 1, 0.01) : \(d).(1 + 7 * d * sqrt(d)) : si.smoo;
resonance   = hslider("resonance", 0.2, 0, 1, 0.01);
env_filter  = hslider("env filter", 0.3, 0, 1, 0.01) : si.smoo;
filter_decay = hslider("filter decay [scale:log] [unit:s]", 0.25, 0.02, 2, 0.001);
sub_level   = hslider("sub", 0.5, 0, 1, 0.01) : si.smoo;
octave      = hslider("octave", 0, -2, 2, 1);
glide       = hslider("glide [unit:ms]", 0, 0, 500, 1) / 1000;
attack      = hslider("attack [scale:log] [unit:ms]", 2, 1, 200, 1) / 1000;
release     = hslider("release [scale:log] [unit:ms]", 80, 5, 2000, 1) / 1000;
vel_sens    = hslider("velocity", 0.5, 0, 1, 0.01);
level       = hslider("level", 0.8, 0, 1, 0.01) : si.smoo;

keys_mode   = hslider("keys [style:menu{'spread':0;'round robin':1;'held':2}]", 0, 0, 2, 1) : int;

// round robin: the four pitches (set by the library) and whether a key is held
last_note   = hslider("note [mono:note]", 36, 0, 127, 1);
key_down    = hslider("gate [mono:gate]", 0, 0, 1, 1);
rr1         = hslider("rr1 [mono:rr1]", 36, 0, 127, 1);     // C2 until a MIDI note arrives
rr2         = hslider("rr2 [mono:rr2]", 36, 0, 127, 1);
rr3         = hslider("rr3 [mono:rr3]", 36, 0, 127, 1);
rr4         = hslider("rr4 [mono:rr4]", 36, 0, 127, 1);
// spread: the four pitches (set by the library)
sp1         = hslider("sp1 [mono:sp1]", 36, 0, 127, 1);
sp2         = hslider("sp2 [mono:sp2]", 36, 0, 127, 1);
sp3         = hslider("sp3 [mono:sp3]", 36, 0, 127, 1);
sp4         = hslider("sp4 [mono:sp4]", 36, 0, 127, 1);
// held: the first held key is the carrier, the next ones operators 2, 3, 4
// (a new key takes the lowest free one)
note1       = hslider("note1 [mono:note1]", 36, 0, 127, 1);     // C2 until a MIDI note arrives
note2       = hslider("note2 [mono:note2]", 36, 0, 127, 1);
note3       = hslider("note3 [mono:note3]", 36, 0, 127, 1);
note4       = hslider("note4 [mono:note4]", 36, 0, 127, 1);
gate1       = hslider("gate1 [mono:gate1]", 0, 0, 1, 1);
gate2       = hslider("gate2 [mono:gate2]", 0, 0, 1, 1);
gate3       = hslider("gate3 [mono:gate3]", 0, 0, 1, 1);
gate4       = hslider("gate4 [mono:gate4]", 0, 0, 1, 1);
velocity    = hslider("velocity in [mono:velocity]", 1, 0, 1, 0.001);
bend        = hslider("bend [midi:pitchwheel] [hidden:1]", 0, -2, 2, 0.001) : si.smoo;

// ---- pitch and gate -------------------------------------------------------

any_gate    = max(max(cv_gate > 0.5, key_down), max(max(gate1, gate2), max(gate3, gate4)));
// a new note: a key goes down, or another key while one is held
started(n, g) = (g > g') | ((n != n') & (g > 0));
new_note    = started(last_note, key_down);
// the envelopes restart on every new note (legato and chords too) and gate
trig        = (any_gate > any_gate') | new_note | started(note1, gate1) | started(note2, gate2)
              | started(note3, gate3) | started(note4, gate4);

rr_note(k)  = ba.selectn(4, k - 1, rr1, rr2, rr3, rr4);
sp_note(k)  = ba.selectn(4, k - 1, sp1, sp2, sp3, sp4);

// held: a key held right now; all released: the last state stays for the release
held(g)     = ba.sAndH(any_gate, g);
held_note(k, n, g) = select2(k == 1, select2(held(g), note1, n), note1);

// operator pitches in semitones (+ v-oct 1 for all, v-oct k on top)
op_note(k, n, g) = ba.selectn(3, keys_mode, sp_note(k), rr_note(k), held_note(k, n, g));
root        = op_note(1, note1, gate1) + 12 * voct1;
op_pitch(k, n, g, v) = op_note(k, n, g) + 12 * (voct1 + v);
glided(p)   = p + 12 * octave : si.smooth(ba.tau2pole(glide)) : +(bend);
hz(p)       = ba.midikey2hz(glided(p));

// ---- envelopes ------------------------------------------------------------

// exponential decay: jumps to 1 on trig, falls to -60 dB in t seconds
decay_env(t) = (\(e).(select2(trig, e * coef, 1.0))) ~ _
with {
    coef = exp(-6.9 / (t * ma.SR));
};
declick     = si.smooth(ba.tau2pole(0.0015));   // 1.5 ms attack against clicks

fenv        = decay_env(filter_decay) : declick;          // opens the filter

ampenv      = select2(vca, 1, en.asr(attack, 1, release, any_gate));
vel_scale   = 1 - vel_sens * (1 - velocity);

// ---- operators and algorithms -----------------------------------------------

phasor(f)   = (+(f / ma.SR) : ma.frac) ~ _;

// sine operator, phase modulated by pm (in cycles)
op(f, pm)   = sin(2 * ma.PI * (phasor(f) + pm));

// a feedback path averaged over two samples against its own oscillation
fb_path(beta) = _ <: (_ + _') * 0.5 * beta;

// routing per mode (control rate): modulation from operator j to k, and the
// level of operators 2..4 as carriers (operator 1 always is one)
//                          stack   Y   twin  fan  branch three parallel
per_mode(k) = ba.selectn(7, mode, k);
c43         = per_mode((1,    0,    1,    1,    1,    0,    0));
c42         = per_mode((0,    0,    0,    1,    0,    0,    0));
c41         = per_mode((0,    1,    0,    1,    0,    1,    0));
c32         = per_mode((1,    1,    0,    0,    0,    0,    0));
c31         = per_mode((0,    0,    0,    0,    1,    1,    0));
c21         = per_mode((1,    1,    1,    0,    1,    1,    0));
g2          = per_mode((0,    0,    0,    1,    0,    0,    1));
g3          = per_mode((0,    0,    1,    1,    0,    0,    1));
g4          = per_mode((0,    0,    0,    0,    0,    0,    1));
// operators 2 and 3 as carriers: their knobs set their level in the mix
// (linear, the depth curve is for modulation) and how strongly they are
// modulated themselves (0..2x)
m2          = 0.25 + 0.75 * mod2_knob : si.smoo;   // never silent as a carrier
m3          = 0.25 + 0.75 * mod3_knob : si.smoo;
m4          = 0.25 + 0.75 * mod4_knob : si.smoo;
carriers    = 1 + g2 * m2 + g3 * m3 + g4 * m4;   // carriers summed: keep the level of one
// a modulator driving several operators or a chain gets less depth each
index_scale = per_mode((0.6,  0.8,  1,    0.7,  0.8,  0.7,  1));

// at most 1.2 cycles (7.5 rad): beyond that the spectrum saturates and the
// knobs stop being heard against each other; less above ~110 Hz
f_root      = hz(root);
keytrack    = min(1, sqrt(110 / f_root));
index_base  = 1.2 * vel_scale * index_scale * keytrack;
i2          = mod2 * index_base;
i3          = mod3 * index_base;
i4          = mod4 * index_base;

// operator frequencies: their pitch times their ratio
f2          = hz(op_pitch(2, note2, gate2, voct2)) * ratio2;
f3          = hz(op_pitch(3, note3, gate3, voct3)) * ratio3;
f4          = hz(op_pitch(4, note4, gate4, voct4)) * ratio4;

// the network on one side (d: detune factor); c1 is the carrier's output
// fed back: carrier and operator 4 modulate each other by cross
network(d, c1) = o1, o2, o3, o4
with {
    o4 = (\(fb).(op(f4 * d, fb + cross * c1))) ~ fb_path(feedback);
    o3 = op(f3 * d, c43 * i4 * o4 * select2(g3 > 0, 1, 2 * m3));
    o2 = op(f2 * d, (c42 * i4 * o4 + c32 * i3 * o3) * select2(g2 > 0, 1, 2 * m2));
    o1 = (\(fb).(op(f_root * d, (c41 * i4 + cross) * o4 + c31 * i3 * o3 + c21 * i2 * o2 + fb))) ~ fb_path(0.8 * feedback);
};
// carrier into operator 4 (one sample later, averaged like feedback)
fm(d)       = network(d) ~ (fb_path(1), !, !, !) : \(o1, o2, o3, o4).((o1 + g2 * m2 * o2 + g3 * m3 * o3 + g4 * m4 * o4) / carriers);
// left and right up to 10 cents apart
detune(side) = 1 + side * 0.003 * width;

// ---- fold, filter, sub ------------------------------------------------------

// wavefolder: a sine of the scaled signal, smooth saturation at drive 0,
// folding back several times at the top; +-1 at most
fold(x)     = sin(ma.PI / 2 * drive * x);

// cutoff: knob + envelope, in the ladder's log domain
filter_n    = cutoff_n + 0.4 * env_filter * fenv : max(0) : min(0.97);
filter_q    = 0.707 + 11 * resonance;
filter      = ve.moogLadder(filter_n, filter_q) : *(1 + 0.8 * resonance);

top(side)   = fm(detune(side)) : fold : fi.dcblocker : filter;
sub         = sin(2 * ma.PI * phasor(f_root / 2)) * sub_level;

gain        = ampenv * level * (0.4 + 0.6 * vel_scale);
side(s)     = (0.6 * top(s) + 0.5 * sub) * gain;

process     = side(-1), side(1), fenv, ampenv;
