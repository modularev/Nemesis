declare name "juno";
declare description "Juno style polysynth: DCO saw/pulse/sub/noise, HPF, 4 pole VCF, ADSR, LFO, chorus I/II";
declare author "Arev Imer";
declare version "0.4";
declare options "[midi:on][nvoices:6]";

// Play it over MIDI (USB or the MIDI jack), stereo out on outputs 1/2.
// Every voice sits at its own place in the stereo field (spread): each
// semitone a fixed position, neighbouring notes far apart (golden ratio
// steps), so chords open up; the chorus then works on both sides.
//
// hardware controls            rev2            rev1
// cutoff resonance env amount  POT_A B C       knob A B C
// release                      POT_D           knob D
// LFO rate, PWM, chorus mode   X Y Z (knob + CV)
// everything else: MIDI CC (numbers in the labels)

import("stdfaust.lib");

// voice parameters set by the polyphony
freq = hslider("freq", 220, 20, 10000, 0.01);
gain = hslider("gain", 0.8, 0, 1, 0.01);
gate = button("gate");

// controls shared by all voices
cutoff    = hslider("Cutoff [hw:POT_A]", 0.6, 0, 1, 0.001) : si.smoo;
resonance = hslider("Resonance [hw:POT_B]", 0.2, 0, 1, 0.001) : si.smoo;
env_amt   = hslider("Env [hw:POT_C]", 0.4, 0, 1, 0.001) : si.smoo;
release   = hslider("Release [hw:POT_D]", 0.3, 0, 1, 0.001) : *(4) : +(0.005);
lfo_rate  = hslider("LFO Rate [hw:CV_X]", 0.3, 0, 1, 0.001) : ^(2) : *(20) : +(0.1) : si.smoo;
pwm       = hslider("PWM [hw:CV_Y]", 0.3, 0, 1, 0.001) : si.smoo;
chorus    = hslider("Chorus [hw:CV_Z]", 0.5, 0, 1, 0.001) : *(3.99) : int; // off, I, II, I+II

attack    = hslider("Attack [midi:ctrl 73]", 0.01, 0, 1, 0.001) : *(3) : +(0.001);
decay     = hslider("Decay [midi:ctrl 75]", 0.3, 0, 1, 0.001) : *(4) : +(0.005);
sustain   = hslider("Sustain [midi:ctrl 79]", 0.7, 0, 1, 0.001);
saw_lvl   = hslider("Saw [midi:ctrl 70]", 1, 0, 1, 0.001) : si.smoo;
pulse_lvl = hslider("Pulse [midi:ctrl 71]", 0, 0, 1, 0.001) : si.smoo;
sub_lvl   = hslider("Sub [midi:ctrl 77]", 0.3, 0, 1, 0.001) : si.smoo;
noise_lvl = hslider("Noise [midi:ctrl 78]", 0, 0, 1, 0.001) : si.smoo;
// HPF frequency from the raw slider, smoothed last (pow once per block)
hpf_freq  = hslider("HPF [midi:ctrl 81]", 0, 0, 1, 0.001) : \(h).(20 * pow(40, h)) : si.smoo;
lfo_vcf   = hslider("LFO VCF [midi:ctrl 76]", 0, 0, 1, 0.001) : si.smoo;
lfo_dco   = hslider("LFO DCO [midi:ctrl 1]", 0, 0, 1, 0.001) : si.smoo; // mod wheel: vibrato
kbd       = hslider("Kybd [midi:ctrl 82]", 0.5, 0, 1, 0.001) : si.smoo;
spread    = hslider("Spread [midi:ctrl 10]", 0.6, 0, 1, 0.01);

lfo = os.lf_triangle(lfo_rate);
env = en.adsr(attack, decay, sustain, release, gate);

// DCO: saw, pulse with PWM (from the LFO, as on the Juno), sub one octave down, noise
pitch = freq * (1 + 0.03 * lfo_dco * lfo);
duty  = 0.5 - 0.45 * pwm * (0.5 + 0.5 * lfo);
dco   = saw_lvl * os.sawtooth(pitch)
      + pulse_lvl * os.pulsetrain(pitch, duty)
      + sub_lvl * os.square(pitch / 2)
      + noise_lvl * no.noise;

// filters: one pole HPF, resonant 4 pole lowpass with env, LFO and key tracking
vcf_oct  = 10 * cutoff + 6 * env_amt * env + 2 * lfo_vcf * lfo + kbd * log(freq / 261.6) / log(2);
// moogLadder: zero delay feedback ladder, stable up to 20 kHz (ve.moog_vcf
// is only valid below SR/6.3 and blew up). Its cutoff is log scaled:
// 0..1 = 20 Hz..20 kHz, i.e. octaves above 20 Hz * log10(2) / 3
vcf_norm = vcf_oct * 0.30103 / 3 : max(0) : min(0.98);
vcf_q    = 0.707 + resonance * 23; // 25 self-oscillates

mono = dco * 0.25 : fi.highpass(1, hpf_freq) : ve.moogLadder(vcf_norm, vcf_q) : *(env * gain);

// pan by note (control rate: freq and spread are sliders): semitone n at
// frac(n * 0.618), equal power, spread 0 = centre
semitone = 12 * log(freq / 27.5) / log(2) + 0.5 : floor;   // rounded: 26.9999 is 27
place    = ma.frac(semitone * 0.618034) - 0.5;          // -0.5 .. 0.5
angle    = (0.5 + place * spread) * ma.PI / 2;
voice    = mono <: *(cos(angle) * sqrt(2)), *(sin(angle) * sqrt(2));   // centre: the old level

// chorus after the voice mix (the 'effect' runs once, not per voice):
// two modulated delays in opposite phase, rates and depths after the Juno 60
chorus_rate  = ba.selectn(4, chorus, 0, 0.5, 0.83, 8);
chorus_depth = ba.selectn(4, chorus, 0, 0.0018, 0.0018, 0.0003);
chorus_lfo   = os.lf_triangle(chorus_rate);
tap(sign)    = de.fdelay(1024, ma.SR * (0.0035 + sign * chorus_depth * chorus_lfo));
stereo(l, r) = l * dry + tap(1, l) * wet, r * dry + tap(-1, r) * wet
with {
    wet = ba.if(chorus > 0, 0.7, 0);
    dry = ba.if(chorus > 0, 0.7, 1);
};

process = voice;
effect = stereo;
