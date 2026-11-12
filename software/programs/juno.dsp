declare name "juno";
declare description "Juno style polysynth: DCO saw/pulse/sub/noise, HPF, 4 pole VCF, ADSR, LFO, chorus I/II";
declare author "Arev Imer";
declare version "0.17";
declare options "[midi:on][nvoices:12]";   // 12 at 48 kHz, 6 at 96 kHz (the library halves them)

// Play it over MIDI (USB or the MIDI jack), stereo out on outputs 1/2.
// Every voice sits at its own place in the stereo field (spread), and notes
// move through the voices, so chords and repeated notes open up; the chorus
// then works on both sides.
//
// hardware controls            rev2                    rev1
// cutoff resonance env amount  X Y Z (knob + CV)       CV X Y Z
// attack decay sustain release POT_A B C D             knob A B C D
// everything else: settings, MIDI CC (numbers in the labels)

import("stdfaust.lib");
// the 106's measured slider curves (cutoff, envelope times, LFO, DCO LFO,
// sub), from Ultramaster KR-106 / Youth106 (GPL-3.0)
import("juno106_curves.lib");

// voice parameters set by the polyphony
freq = hslider("freq", 220, 20, 10000, 0.01);
gain = hslider("gain", 0.8, 0, 1, 0.01);
gate = button("gate");

// controls shared by all voices, smoothed in every voice: a voice is only
// computed while it sounds, so its smoother would start a note from the
// value it had at its last note (a glide, heard on the resonant filter);
// at a note start it takes the value at once
note_on   = gate > gate';
vs(x)     = x : si.smooth(ba.if(note_on, 0, ba.tau2pole(0.005)));
cutoff    = hslider("Cutoff [hw:CV_X]", 0.6, 0, 1, 0.001) : vs;
res_raw   = hslider("Resonance [hw:CV_Y]", 0.2, 0, 1, 0.001);
resonance = res_raw : vs;
env_amt   = hslider("Env [hw:CV_Z]", 0.4, 0, 1, 0.001) : vs;
// envelope times as measured on a 106 (seconds; control rate): attack
// 1 ms .. 3.3 s; decay and release to -20 dB, 4 ms .. 22 s
attack_s(v) = pow(2, curve(T_ATTACK_LOG2MS, 33, v)) * 0.001;
decrel_s(v) = pow(2, curve(T_DECREL_LOG2MS, 33, v)) * 0.001;
release   = hslider("Release [hw:POT_D]", 0.4, 0, 1, 0.001) : decrel_s;
// 0.04 .. 30 Hz, the 106's measured curve
lfo_rate  = hslider("LFO Rate", 0.4, 0, 1, 0.001) : \(v).(pow(2, curve(T_LFO_LOG2HZ, 33, v))) : vs;
pwm       = hslider("PWM", 0.3, 0, 1, 0.001) : vs;
chorus    = hslider("Chorus [style:menu{'off':0;'I':1;'II':2;'I+II':3}]", 1, 0, 3, 1) : int;
// pitch bend: +-2 semitones (the ratio once per block, then smoothed)
bend      = hslider("bend [midi:pitchwheel] [hidden:1]", 0, -2, 2, 0.001) : \(b).(pow(2, b / 12)) : vs;

attack    = hslider("Attack [hw:POT_A] [midi:ctrl 73]", 0.01, 0, 1, 0.001) : attack_s;
decay     = hslider("Decay [hw:POT_B] [midi:ctrl 75]", 0.4, 0, 1, 0.001) : decrel_s;
sustain   = hslider("Sustain [hw:POT_C] [midi:ctrl 79]", 0.7, 0, 1, 0.001);
saw_lvl   = hslider("Saw [midi:ctrl 70]", 1, 0, 1, 0.001) : vs;
pulse_lvl = hslider("Pulse [midi:ctrl 71]", 0, 0, 1, 0.001) : vs;
// sub and noise levels as measured (the noise: a soft knee fit)
sub_lvl   = hslider("Sub [midi:ctrl 77]", 0.3, 0, 1, 0.001) : curve(T_SUB, 33) : vs;
noise_lvl = hslider("Noise [midi:ctrl 78]", 0, 0, 1, 0.001)
          : \(t).(ba.if(t <= 0, 0, 1.0632 * (sqrt((t - 0.0594) * (t - 0.0594) + 0.0146 * 0.0146) + t - 0.0594) * 0.5)) : vs;
// HPF after the voice mix (global, as on the Juno): in the effect. The
// 106's four positions: 0 boosts the bass (organ), 1 passes the sound
// unprocessed, 2 and 3 cut from 236 / 754 Hz (first order)
hpf_pos   = hslider("HPF [style:menu{'0 boost':0;'1 flat':1;'2':2;'3':3}] [midi:ctrl 81]", 1, 0, 3, 1) : int;
lfo_vcf   = hslider("LFO VCF [midi:ctrl 76]", 0, 0, 1, 0.001) : vs;
lfo_dco   = hslider("LFO DCO [midi:ctrl 1]", 0, 0, 1, 0.001); // mod wheel: vibrato (its curve below, then smoothed)
kbd       = hslider("Kybd [midi:ctrl 82]", 0.5, 0, 1, 0.001) : vs;
spread    = hslider("Spread [midi:ctrl 10]", 0.6, 0, 1, 0.01);
// the rest of a Juno-106 patch
// the LFO fades in after a note starts: 0 .. 4.4 s as measured
lfo_delay = hslider("LFO Delay", 0, 0, 1, 0.001) : \(v).(curve(T_LFO_DELAY_MS, 33, v) * 0.001);
// VCA level: +-10 dB around 0.5, as the 106
level     = hslider("Level", 0.5, 0, 1, 0.001) : \(v).(pow(10, v - 0.5)) : vs;
range     = hslider("range [style:menu{'16':0;'8':1;'4':2}]", 1, 0, 2, 1);
pwm_man   = hslider("PWM mode [style:menu{'LFO':0;'manual':1}]", 0, 0, 1, 1);
env_neg   = hslider("env polarity [style:menu{'+':0;'-':1}]", 0, 0, 1, 1);
vca_gate  = hslider("VCA [style:menu{'env':0;'gate':1}]", 0, 0, 1, 1);
// presets: lines of /NEMESIS/juno.PRE on the SD card (the 106's factory
// patches, made by software/tools/juno106_presets.py)
patch     = hslider("patch [action:preset] [home:1] [random:0]", 0, 0, 127, 1);   // [home:1]: the encoder on the home screen
// the 106's assign modes, run by the polyphony ([poly:mode]): poly 1 (release
// tails ring out), poly 2 (only the last notes keep their release), unison
// (every voice on the last note held)
keys      = hslider("keys [poly:mode] [style:menu{'poly 1':0;'poly 2':1;'unison':2}]", 0, 0, 2, 1);
// analog drift: every voice slightly detuned and its filter a bit off, a
// small random detune per note, and a slow wander per voice
drift_on  = hslider("drift [style:menu{'off':0;'on':1}]", 1, 0, 1, 1);
// resonance takes bass away (the ladder's passband gain is 1 / (1 + k));
// 1 keeps it, 0 as the original
bass_comp = hslider("bass comp", 1, 0, 1, 0.01) : vs;

lfo = os.lf_triangle(lfo_rate);
// the LFO delay: per voice, from the note's start (for DCO and VCF; the PWM's
// LFO runs freely)
since     = (+(1) : *(1 - note_on)) ~ _;
lfo_fade  = min(1, since / max(1, lfo_delay * ma.SR));
lfo_d     = lfo * lfo_fade;

// ---- drift (per voice: [poly:voice] numbers the voices, see the pan below)
hash(k)    = ma.frac(sin(k * 12.9898 + 78.233) * 43758.5453) * 2 - 1;   // -1..1, fixed per voice
note_rand  = ba.sAndH(note_on, no.noise);             // -1..1, new at every note on
wander     = 0.6 * os.lf_triangle(0.11 + 0.047 * voice_no) + 0.4 * os.lf_triangle(0.23 + 0.061 * voice_no);
drift_ct   = drift_on * (3.0 * hash(voice_no) + 2.0 * note_rand + 2.0 * wander);       // cents: ~ +-3, at most +-7
// the filter drifts like the DCOs (in cents, its own offsets per voice): a
// self-oscillating filter stays playable
drift_oct  = drift_on * (3.0 * hash(voice_no + 31) + 2.0 * note_rand + 2.0 * wander) / 1200;
// the 106's envelope (its firmware): a straight attack, decay and release
// exponential towards the sustain level / zero; their times are to -20 dB,
// so ln(10) time constants
env_rc(t) = exp(-2.302585 / max(1, t * ma.SR));        // per sample factor (control rate)
env = loop ~ (_, _) : (_, !)
with {
    loop(e, ph) = e2, ph2
    with {
        ph2 = ba.if(gate > 0.5, ba.if(note_on, 0, ba.if(e >= 1, 1, ph)), 2);  // 0 attack, 1 decay, 2 release
        e2  = ba.selectn(3, ph2, min(1, e + 1 / max(1, attack * ma.SR)),
                         sustain + (e - sustain) * env_rc(decay), e * env_rc(release));
    };
};

// DCO: one ramp for saw and pulse, a ramp at half the rate for the sub
// (one octave down, flipping with the main ramp as the DCO's divider), noise.
// polyBLEP: the steps' aliasing removed in the two samples around them
// (was os.sawtooth/pulsetrain/square: five DPW saws with divisions each)
// the DCO's LFO depth: the 106's curve, up to +-4 semitones; 2^y for
// |y| <= 1/3 as a polynomial (a pow per sample costs)
exp2s(y) = 1 + t * (1 + t * (0.5 + t * 0.1666667)) with { t = y * 0.6931472; };
dco_lfo_st = lfo_dco : curve(T_DCO_LFO_ST, 33) : vs;    // from the raw slider: the table once per block
pitch = freq * bend * ba.selectn(3, range, 0.5, 1, 2)
      * exp2s(dco_lfo_st / 12 * lfo_d) * (1 + drift_ct * 0.000577623);   // 2^(c/1200) ~ 1 + c ln2/1200
duty  = 0.5 - 0.45 * pwm * ba.if(pwm_man > 0.5, 1, 0.5 + 0.5 * lfo);
dt    = pitch / ma.SR;
idt   = ma.SR / pitch;
ramp  = (+(dt) : ma.frac) ~ _;
ramp2 = (+(0.5 * dt) : ma.frac) ~ _;
// the correction for a step of +2 at phase 0, for a ramp moving step d per sample
blep(d, id, t) = ba.if(t < d, a + a - a * a - 1, 0) + ba.if(t > 1 - d, b * b + b + b + 1, 0)
with { a = t * id; b = (t - 1) * id; };
saw   = 2 * ramp - 1 - blep(dt, idt, ramp);
// the pulse is the 106's comparator on the ramp: its narrow low part at
// the ramp's start, so it is in phase with the saw (inverted, the two
// partly cancelled: factory A15 with both was 9 dB too quiet)
pulse = ba.if(ramp < duty, -1, 1) - blep(dt, idt, ramp) + blep(dt, idt, ma.frac(ramp + 1 - duty))
      - (1 - 2 * duty);                                   // without its DC
sub   = ba.if(ramp2 < 0.5, 1, -1) + blep(0.5 * dt, 2 * idt, ramp2) - blep(0.5 * dt, 2 * idt, ma.frac(ramp2 + 0.5));
// the 106's mix (Youth106: saw 0.6, pulse 0.5, sub 0.75, noise 1.2), scaled
// so that the saw alone keeps its level
dco   = (0.6 * saw_lvl * saw + 0.5 * pulse_lvl * pulse + 0.75 * sub_lvl * sub + 1.2 * noise_lvl * no.noise) / 0.6;

// VCF: 4 pole lowpass as the 80017A/IR3109's four OTA stages with global
// feedback: a zero delay feedback (TPT) ladder (Zavalishin), its input soft
// saturated like an OTA (also bounds high resonance).
// The cutoff as the 106's firmware adds it up, in slider units (1 = the
// slider's travel): slider + env amount x envelope + LFO (3.5 octaves) +
// key follow (1:1 around middle C) + drift, then the measured slider -> Hz
// curve of a voice card (5.7 Hz .. 50 kHz, ~14 octaves). The stage
// coefficient g = tan(w) / (1 + tan(w)) comes from a table over 0..1 (256
// steps, filled for the sample rate), interpolated: no pow/tan per sample.
ST_PER_UNIT = 169.33;                                   // semitones per slider unit (firmware: 96 / 16256 per semitone)
vcf_x   = cutoff + env_amt * env * (1 - 2 * env_neg) + lfo_vcf * lfo_d * 42 / ST_PER_UNIT
        + kbd * 12 * log(freq / 261.63) / log(2) / ST_PER_UNIT + drift_ct / 100 / ST_PER_UNIT
        + peak_pull
        : max(0) : min(1);
// resonance pulls the 106's cutoff up (KR-106's fit to a hardware noise
// grid: x exp(m(k)), up to ~ +0.25 octaves at medium resonance), here as
// slider units: m / ln 2 octaves = m x 12 / ln 2 / ST_PER_UNIT
peak_pull = res_raw : res_k : pull : vs
with {
    pull(k) = m0 * ba.if(k < 1, max(k, 0), 1) * ba.if(k > 3.7, max(0, min(1, (4.4 - k) / 0.7)), 1) * 0.102237
    with { m0 = 0.20646 + k * (-0.04140 + k * (0.00602 - k * 0.00127)); };
};
// the measured slider -> log2(Hz) curve as a polynomial (degree 9, within
// 0.012 octaves of the 1024 measured points): FAUST 2.88 doesn't fill a
// table read while another table is generated (it stayed 0: 1 Hz)
vcf_log2hz(x) = 2.512921 + x * (14.693375 + x * (-14.099046 + x * (155.254468 + x * (-894.285764
              + x * (2893.606986 + x * (-5454.787850 + x * (5941.911914 + x * (-3460.546967 + x * 831.378569))))))));
STEPS   = 256;
TN      = STEPS + 2;
gtab(i) = rdtable(TN, ba.time : gen, i)
with {
    gen(k) = w / (1 + w)
    with {
        fc = min(pow(2, vcf_log2hz(k / STEPS)), 0.45 * ma.SR);
        w  = tan(ma.PI * fc / ma.SR);
    };
};
vcf_g   = g0 + (x - i) * (gtab(i + 1) - g0)
with {
    x  = vcf_x * STEPS;
    i  = int(x);
    g0 = gtab(i);
};
// resonance -> feedback: the 106's measured curve (a quartic fit to a
// hardware sweep), plus a boost above 0.9 into self-oscillation (k > 4);
// from the raw slider, smoothed after (once per block, not per sample)
res_k(r) = r * (4.6265 + r * (4.3866 + r * (-8.9869 + r * 3.3902))) + max(0, r - 0.9) * 10;
vcf_k   = res_raw : res_k : vs;

// tanh(x) ~ x (27 + x^2) / (27 + 9 x^2), exact at 0, 1 at |x| = 3: the OTA
// input; its limit sets the self-oscillation's level (0.5 was quieter than
// a note)
SAT_LIMIT = 1.0;
tanh_(x) = y * (27 + y * y) / (27 + 9 * y * y) with { y = max(-3, min(3, x)); };
sat(x)   = SAT_LIMIT * tanh_(x / SAT_LIMIT);
thermal  = 0.0001 * no.noise;                            // -80 dB: self-oscillation starts from it
// on the 106 the filter oscillates all the time and the VCA opens on it; a
// voice here starts from rest (or where it stopped), so a note start kicks
// the filter (resonance^4: only near self-oscillation), else it took
// hundreds of ms to build up (Resonance Funk stayed silent)
// (a 5 ms noise burst: a single sample gave a low cutoff too little energy)
kick     = (since < 0.005 * ma.SR) * 0.3 * no.noise * resonance * resonance * resonance * resonance;

// feedback of (output - c * input): passband gain (1 + k c) / (1 + k), the
// resonance peak unchanged; u = x - k (y - c x) with y = G u + S
ladder(g, k, c, x) = loop ~ si.bus(4) : (!, !, !, !, _)
with {
    loop(s1, s2, s3, s4) = v1 + lp1, v2 + lp2, v3 + lp3, v4 + lp4, lp4
    with {
        a   = 1 - g;
        G   = g * g * g * g;
        S   = a * (s4 + g * (s3 + g * (s2 + g * s1)));   // the stages' state seen at the output
        u   = (x * (1 + k * c) - k * S) / (1 + k * G) : sat;
        v1  = g * (u - s1);
        lp1 = v1 + s1;
        v2  = g * (lp1 - s2);
        lp2 = v2 + s2;
        v3  = g * (lp2 - s3);
        lp3 = v3 + s3;
        v4  = g * (lp3 - s4);
        lp4 = v4 + s4;
    };
};

// VCA: the envelope, or the gate (2 ms edges, organ like)
vca  = ba.if(vca_gate > 0.5, gate : si.smooth(ba.tau2pole(0.002)), env);
// (patch is only read by the library: attached, else FAUST drops it)
mono = dco * 0.25 + thermal + kick : ladder(vcf_g, vcf_k, bass_comp) : *(vca * gain * level) : attach(_, patch) : attach(_, keys);

// pan by voice: every voice has its own place in the stereo field (the
// polyphony numbers them, [poly:voice]), neighbouring voices far apart
// (golden ratio steps from the centre); notes move through the voices, so
// even the same note wanders. Spread 0 = centre. In FAUST's own polyphony
// every voice is number 0: the centre.
voice_no = hslider("voice [poly:voice] [hidden:1]", 0, 0, 15, 1);
place    = ma.frac(voice_no * 0.618034 + 0.5) - 0.5;    // -0.5 .. 0.5
angle    = (0.5 + place * spread) * ma.PI / 2;
voice    = mono <: *(cos(angle) * sqrt(2)), *(sin(angle) * sqrt(2));   // centre: the old level

// chorus after the voice mix (the 'effect' runs once, not per voice): two
// BBD delays swept in opposite phase by one triangle, as measured on a 106
// (Youth106/KR-106): 3.3 ms +- 2.13 ms at 0.514 Hz (I) / 0.835 Hz (II), the
// Juno-60's I+II 7.85 Hz +- 0.236 ms; dry 0.863, wet 1.257
chorus_rate  = ba.selectn(4, chorus, 0, 0.514, 0.835, 7.85);
chorus_depth = ba.selectn(4, chorus, 0, 0.00213, 0.00213, 0.000236);
chorus_lfo   = os.lf_triangle(chorus_rate);
// the BBD's output filter: 9.66 kHz, 12 dB/oct, on the chorus delays only
tap(sign)    = de.fdelay(1024, ma.SR * (0.0033 + sign * chorus_depth * chorus_lfo)) : fi.lowpass(2, 9661);

// HPF position 0, the bass boost: the 106's two stage circuit (2 poles at
// 59/72 Hz, 2 zeros at 72/170 Hz, +10.5 dB at DC), bilinear
boost = fi.tf2(b0, b1, b2, a1, a2)
with {
    t1z = 47e3 * 0.047e-6;  t1p = 47e3 * (0.047e-6 + 0.01e-6);
    t2z = (10e3 * 100e3 / 110e3) * 0.022e-6;  t2p = 100e3 * 0.022e-6;
    ag  = (47e3 / 220e3) * (1 + 100e3 / 10e3);  dr = 1;   // R45/R44 x stage 2 gain, R45/R43
    d1 = t1p + t2p;  d2 = t1p * t2p;
    n0 = dr + ag;  n1 = dr * d1 + ag * (t1z + t2z);  n2 = dr * d2 + ag * t1z * t2z;
    k  = 2 * ma.SR;  a0 = 1 + d1 * k + d2 * k * k;
    b0 = (n0 + n1 * k + n2 * k * k) / a0;  b1 = 2 * (n0 - n2 * k * k) / a0;  b2 = (n0 - n1 * k + n2 * k * k) / a0;
    a1 = 2 * (1 - d2 * k * k) / a0;  a2 = (1 - d1 * k + d2 * k * k) / a0;
};
hpf(x) = ba.selectn(4, hpf_pos, boost(x), x, fi.highpass(1, 236, x), fi.highpass(1, 754, x));

chorused(lv, rv) = l * dry + tap(1, l) * wet, r * dry + tap(-1, r) * wet
with {
    l = lv : hpf;   // the HPF after the voices
    r = rv : hpf;
    wet = ba.if(chorus > 0, 1.257, 0);
    dry = ba.if(chorus > 0, 0.863, 1);
};

// output level: -16 dB puts the factory patches' chords (6 notes) at a
// median peak of -6 dBFS, 90 % below -1 dBFS (they were +10 dBFS: nearly
// every patch clipped); Volume as the 106's knob. (A limiter here sounded
// bad: lower Volume or the patch's Level when chords clip.)
volume = hslider("Volume [unit:dB]", 0, -30, 10, 0.5) : ba.db2linear : *(0.1585) : si.smoo;
stereo = chorused : *(volume), *(volume);

process = voice;
effect = stereo;
