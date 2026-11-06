declare name "growler";
declare description "Mono FM bass voice for aggressive, growling basses: feedback FM, wavefolder, ladder filter, transport synced wobble, clean sub; MIDI or V/Oct + gate";
declare author "Arev Imer";
declare version "0.1";
declare options "[midi:on]";
declare audio_outputs "1 2";   // outs 3/4 are CVs (output setting leaves them exact)

// One voice, the same on out 1 and 2 (bass stays mono compatible):
//
//   modulator (ratio, self feedback) --index--> carrier --> wavefolder (drive)
//      --> 24 dB ladder filter --+--> amp envelope --> out 1/2
//   sub sine (one octave down, clean, bypasses fold and filter) --+
//
// growl sets the FM index: the brightness and snarl. A decay envelope
// (restarted on every note, legato too) pushes index and cutoff on the
// attack. The wobble LFO moves index and cutoff together (the "talking"
// bass): synced to the transport while it runs (system settings), at the
// transport tempo and restarted on every note while it is stopped. The index
// drops above ~110 Hz so high notes don't turn to noise. Feedback feeds both
// operators back into themselves: from sines towards saws (heard without
// growl too) and, at the top, into noise.
//
// in: none; V/Oct on CV A (rev2, calibrated) or CV E (rev1), gate on CV B
//     (rev2, > 4 V or 2.5 V with the 0-5 V range) or CV F (rev1)
// out 1/2: audio (mono)
// out 3: wobble LFO, 0..5 V        out 4: amp envelope, 0..5 V
//
// hardware controls (rev2)                          rev1
// growl, cutoff, wobble rate  X Y Z (knob + CV)     CV X Y Z
// V/Oct, gate                 CV A, CV B            CV E, CV F
// ratio, feedback             POT A, POT B          CV A, B
// drive, decay                POT C, POT D (+ CV C, D)   CV C, D
// settings (turn the encoder): wobble depth (MIDI mod wheel), env filter,
//   resonance, sub, octave, glide, attack, release, velocity, level
// MIDI: note, velocity, pitch bend (+-2 semitones), CC 1 wobble depth

import("stdfaust.lib");
import("nemesis.lib");

// ---- controls -------------------------------------------------------------

// curves are computed from the raw sliders (control rate) and smoothed last
growl       = hslider("growl [hw:CV_X]", 0.5, 0, 1, 0.001) : \(g).(g * sqrt(g)) : si.smoo;
cutoff_n    = hslider("cutoff [hw:CV_Y] [scale:log] [unit:Hz]", 600, 30, 12000, 1)
              : \(c).(log10(c / 20) / 3) : si.smoo;   // ladder's 0..1 = 20 Hz..20 kHz
wobble_sel  = hslider("wobble [hw:CV_Z] [style:menu{'off':0;'1/1':1;'1/2':2;'1/4':3;'1/4T':4;'1/8':5;'1/8T':6;'1/16':7;'1/16T':8;'1/32':9}]", 0, 0, 9.999, 1) : int;
voct        = hslider("v-oct [rev2:CV_A] [rev1:CV_E] [volts:1] [unit:V]", 0, -10, 10, 0.001);
cv_gate     = checkbox("cv gate [rev2:CV_B] [rev1:CV_F]");
ratio_sel   = hslider("ratio [hw:POT_A] [style:menu{'1/2':0;'1':1;'3/2':2;'2':3;'3':4;'4':5;'5':6;'7':7}]", 1, 0, 7.999, 1) : int;
feedback    = hslider("feedback [hw:POT_B]", 0.2, 0, 1, 0.001) : \(f).(0.32 * f * f) : si.smoo;
drive       = hslider("drive [hw:POT_C] [cv:CV_C]", 0.3, 0, 1, 0.001) : \(d).(1 + 7 * d * sqrt(d)) : si.smoo;
decay       = hslider("decay [hw:POT_D] [cv:CV_D] [scale:log] [unit:s]", 0.25, 0.02, 2, 0.001);

wob_depth   = hslider("wobble depth [midi:ctrl 1]", 0.6, 0, 1, 0.01) : si.smoo;
env_filter  = hslider("env filter", 0.5, 0, 1, 0.01) : si.smoo;
resonance   = hslider("resonance", 0.3, 0, 1, 0.01);
sub_level   = hslider("sub", 0.5, 0, 1, 0.01) : si.smoo;
octave      = hslider("octave", 0, -2, 2, 1);
glide       = hslider("glide [unit:ms]", 0, 0, 500, 1) / 1000;
attack      = hslider("attack [scale:log] [unit:ms]", 2, 1, 200, 1) / 1000;
release     = hslider("release [scale:log] [unit:ms]", 80, 5, 2000, 1) / 1000;
vel_sens    = hslider("velocity", 0.5, 0, 1, 0.01);
level       = hslider("level", 0.8, 0, 1, 0.01) : si.smoo;

note        = hslider("note [mono:note]", 36, 0, 127, 1);       // C2 until a MIDI note arrives
midi_gate   = hslider("gate [mono:gate]", 0, 0, 1, 1);
velocity    = hslider("velocity in [mono:velocity]", 1, 0, 1, 0.001);
bend        = hslider("bend [midi:pitchwheel] [hidden:1]", 0, -2, 2, 0.001) : si.smoo;

// ---- pitch and gate -------------------------------------------------------

gate        = max(midi_gate, cv_gate > 0.5);
// attack on a new gate and on every new note while held (legato)
trig        = (gate > gate') | ((note != note') & (gate > 0));

semitones   = note + 12 * octave + 12 * voct : si.smooth(ba.tau2pole(glide)) : +(bend);
freq        = ba.midikey2hz(semitones);

// ---- envelopes ------------------------------------------------------------

// decay envelope for index and cutoff: jumps to 1 on trig, falls with
// decay (time to -60 dB), 1.5 ms attack against clicks
modenv      = (\(e).(select2(trig, e * coef, 1.0))) ~ _ : si.smooth(ba.tau2pole(0.0015))
with {
    coef = exp(-6.9 / (decay * ma.SR));
};
ampenv      = en.asr(attack, 1, release, gate);
vel_scale   = 1 - vel_sens * (1 - velocity);

// ---- wobble ---------------------------------------------------------------

// ticks per LFO cycle (24 per quarter)
wob_ticks   = ba.selectn(10, wobble_sel, 1, 96, 48, 24, 16, 12, 8, 6, 4, 3);
wob_on      = wobble_sel > 0;
// free running at the transport tempo, restarted by notes, while stopped
wob_free    = (*(1 - trig) : +(nm_bpm / 60 * 24 / wob_ticks / ma.SR) : ma.frac) ~ _;
wob_phase   = select2(nm_running > 0, wob_free, ma.frac(nm_position / wob_ticks));
wob         = 0.5 - 0.5 * cos(2 * ma.PI * wob_phase);   // 0 .. 1, starts closed
wob_amount  = wob_on * wob_depth;

// ---- voice ----------------------------------------------------------------

phasor(f)   = (+(f / ma.SR) : ma.frac) ~ _;

// sine operator with self feedback (phase modulation, in cycles), the
// feedback path averaged over two samples against its own oscillation
fb_op(f, beta) = loop ~ (_ <: (_ + _') * 0.5 * beta)
with {
    loop(fb) = sin(2 * ma.PI * (phasor(f) + fb));
};

ratio       = ba.selectn(8, ratio_sel, 0.5, 1, 1.5, 2, 3, 4, 5, 7);
keytrack    = min(1, sqrt(110 / freq));
index       = growl * 3 * (0.35 + 0.65 * modenv) * (1 - wob_amount * (1 - wob)) * keytrack * vel_scale;

modulator   = fb_op(freq * ratio, feedback);
// carrier: phase modulated by the modulator and by its own output
carrier     = loop ~ (_ <: (_ + _') * 0.5 * 0.8 * feedback)
with {
    loop(fb) = sin(2 * ma.PI * (phasor(freq) + index * modulator + fb));
};

// wavefolder: a sine of the scaled signal, smooth saturation at drive 0,
// folding back several times at the top; +-1 at most
fold(x)     = sin(ma.PI / 2 * drive * x);

// cutoff: knob + decay envelope + wobble, in the ladder's log domain
filter_n    = cutoff_n + 0.4 * env_filter * modenv + 0.35 * wob_amount * (wob - 0.5) : max(0) : min(0.97);
filter_q    = 0.707 + 11 * resonance;
filter      = ve.moogLadder(filter_n, filter_q) : *(1 + 0.8 * resonance);

top         = carrier : fold : fi.dcblocker : filter;
sub         = sin(2 * ma.PI * phasor(freq / 2)) * sub_level;

voice       = (0.6 * top + 0.5 * sub) * ampenv * level * (0.4 + 0.6 * vel_scale);

process     = voice <: _, _, (wob * wob_on), ampenv;
