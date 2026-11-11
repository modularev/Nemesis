declare name "juno";
declare description "Juno style polysynth: DCO saw/pulse/sub/noise, HPF, 4 pole VCF, ADSR, LFO, chorus I/II";
declare author "Arev Imer";
declare version "0.12";
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
resonance = hslider("Resonance [hw:CV_Y]", 0.2, 0, 1, 0.001) : vs;
env_amt   = hslider("Env [hw:CV_Z]", 0.4, 0, 1, 0.001) : vs;
// envelope times exponential like the Juno-106's sliders: attack 1.5 ms ..
// 3 s, decay and release 1.5 ms .. 12 s (control rate: pow once per block)
release   = hslider("Release [hw:POT_D]", 0.6, 0, 1, 0.001) : \(v).(0.0015 * pow(8000, v));
// 0.1 .. 30 Hz as the 106 (owner's manual), exponential
lfo_rate  = hslider("LFO Rate", 0.4, 0, 1, 0.001) : \(v).(0.1 * pow(300, v)) : vs;
pwm       = hslider("PWM", 0.3, 0, 1, 0.001) : vs;
chorus    = hslider("Chorus [style:menu{'off':0;'I':1;'II':2;'I+II':3}]", 1, 0, 3, 1) : int;
// pitch bend: +-2 semitones (the ratio once per block, then smoothed)
bend      = hslider("bend [midi:pitchwheel] [hidden:1]", 0, -2, 2, 0.001) : \(b).(pow(2, b / 12)) : vs;

attack    = hslider("Attack [hw:POT_A] [midi:ctrl 73]", 0.01, 0, 1, 0.001) : \(v).(0.0015 * pow(2000, v));
decay     = hslider("Decay [hw:POT_B] [midi:ctrl 75]", 0.6, 0, 1, 0.001) : \(v).(0.0015 * pow(8000, v));
sustain   = hslider("Sustain [hw:POT_C] [midi:ctrl 79]", 0.7, 0, 1, 0.001);
saw_lvl   = hslider("Saw [midi:ctrl 70]", 1, 0, 1, 0.001) : vs;
pulse_lvl = hslider("Pulse [midi:ctrl 71]", 0, 0, 1, 0.001) : vs;
sub_lvl   = hslider("Sub [midi:ctrl 77]", 0.3, 0, 1, 0.001) : vs;
noise_lvl = hslider("Noise [midi:ctrl 78]", 0, 0, 1, 0.001) : vs;
// HPF after the voice mix (global, as on the Juno): in the effect. The
// 106's four positions: 0 boosts the bass (organ), 1 passes the sound
// unprocessed, 2 and 3 cut more (the frequencies and the boost are
// estimates: the manual gives none)
hpf_pos   = hslider("HPF [style:menu{'0 boost':0;'1 flat':1;'2':2;'3':3}] [midi:ctrl 81]", 1, 0, 3, 1) : int;
hpf_freq  = ba.selectn(4, hpf_pos, 20, 20, 240, 720) : si.smoo;
hpf_boost = ba.if(hpf_pos == 0, 2, 1) : si.smoo;           // +6 dB below ~120 Hz
lfo_vcf   = hslider("LFO VCF [midi:ctrl 76]", 0, 0, 1, 0.001) : vs;
lfo_dco   = hslider("LFO DCO [midi:ctrl 1]", 0, 0, 1, 0.001) : vs; // mod wheel: vibrato
kbd       = hslider("Kybd [midi:ctrl 82]", 0.5, 0, 1, 0.001) : vs;
spread    = hslider("Spread [midi:ctrl 10]", 0.6, 0, 1, 0.01);
// the rest of a Juno-106 patch
lfo_delay = hslider("LFO Delay", 0, 0, 1, 0.001);        // the LFO fades in after a note starts, 0 .. 3 s
level     = hslider("Level", 0.5, 0, 2, 0.001) : vs;     // VCA level, 0.5 = unity (presets levelled up to 2)
range     = hslider("range [style:menu{'16':0;'8':1;'4':2}]", 1, 0, 2, 1);
pwm_man   = hslider("PWM mode [style:menu{'LFO':0;'manual':1}]", 0, 0, 1, 1);
env_neg   = hslider("env polarity [style:menu{'+':0;'-':1}]", 0, 0, 1, 1);
vca_gate  = hslider("VCA [style:menu{'env':0;'gate':1}]", 0, 0, 1, 1);
// presets: lines of /NEMESIS/juno.PRE on the SD card (the 106's factory
// patches, made by software/tools/juno106_presets.py)
patch     = hslider("patch [action:preset] [random:0]", 0, 0, 127, 1);
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
lfo_fade  = min(1, since / max(1, lfo_delay * lfo_delay * 3 * ma.SR));
lfo_d     = lfo * lfo_fade;

// ---- drift (per voice: [poly:voice] numbers the voices, see the pan below)
hash(k)    = ma.frac(sin(k * 12.9898 + 78.233) * 43758.5453) * 2 - 1;   // -1..1, fixed per voice
note_rand  = ba.sAndH(note_on, no.noise);             // -1..1, new at every note on
wander     = 0.6 * os.lf_triangle(0.11 + 0.047 * voice_no) + 0.4 * os.lf_triangle(0.23 + 0.061 * voice_no);
drift_ct   = drift_on * (3.0 * hash(voice_no) + 2.0 * note_rand + 2.0 * wander);       // cents: ~ +-3, at most +-7
// the filter drifts like the DCOs (in cents, its own offsets per voice): a
// self-oscillating filter stays playable
drift_oct  = drift_on * (3.0 * hash(voice_no + 31) + 2.0 * note_rand + 2.0 * wander) / 1200;
env = en.adsr(attack, decay, sustain, release, gate);

// DCO: one ramp for saw and pulse, a ramp at half the rate for the sub
// (one octave down, flipping with the main ramp as the DCO's divider), noise.
// polyBLEP: the steps' aliasing removed in the two samples around them
// (was os.sawtooth/pulsetrain/square: five DPW saws with divisions each)
pitch = freq * bend * ba.selectn(3, range, 0.5, 1, 2)
      * (1 + 0.03 * lfo_dco * lfo_d) * (1 + drift_ct * 0.000577623);   // 2^(c/1200) ~ 1 + c ln2/1200
duty  = 0.5 - 0.45 * pwm * ba.if(pwm_man > 0.5, 1, 0.5 + 0.5 * lfo);
dt    = pitch / ma.SR;
idt   = ma.SR / pitch;
ramp  = (+(dt) : ma.frac) ~ _;
ramp2 = (+(0.5 * dt) : ma.frac) ~ _;
// the correction for a step of +2 at phase 0, for a ramp moving step d per sample
blep(d, id, t) = ba.if(t < d, a + a - a * a - 1, 0) + ba.if(t > 1 - d, b * b + b + b + 1, 0)
with { a = t * id; b = (t - 1) * id; };
saw   = 2 * ramp - 1 - blep(dt, idt, ramp);
pulse = ba.if(ramp < duty, 1, -1) + blep(dt, idt, ramp) - blep(dt, idt, ma.frac(ramp + 1 - duty))
      - (2 * duty - 1);                                   // without its DC
sub   = ba.if(ramp2 < 0.5, 1, -1) + blep(0.5 * dt, 2 * idt, ramp2) - blep(0.5 * dt, 2 * idt, ma.frac(ramp2 + 0.5));
dco   = saw_lvl * saw + pulse_lvl * pulse + sub_lvl * sub + noise_lvl * no.noise;

// VCF: 4 pole lowpass as the 80017A/IR3109's four OTA stages with global
// feedback: a zero delay feedback (TPT) ladder (Zavalishin), its input soft
// saturated like an OTA (also bounds high resonance). The cutoff in octaves
// above 20 Hz (env, LFO, key tracking per sample) gives the stage
// coefficient g = tan(w) / (1 + tan(w)) from a table (16 points per octave,
// filled for the sample rate), interpolated: no pow/tan per sample.
// The knob spans 20 Hz .. 54 kHz (11.4 octaves, the Juno's range); the
// table stops at 0.45 SR (21.6 kHz at 48 kHz, 43.2 kHz at 96 kHz), so
// every rate uses all of its spectrum.
VCF_OCT = 11.4;
// the envelope at full depth sweeps the whole range, as the 106's (6
// octaves left the factory brass at cutoff 0 / env 94 dark and quiet)
vcf_oct = VCF_OCT * cutoff + VCF_OCT * env_amt * env * (1 - 2 * env_neg) + 2 * lfo_vcf * lfo_d + kbd * log(freq / 261.6) / log(2) + drift_oct
        : max(0) : min(VCF_OCT);
STEPS   = 16;
TN      = 12 * STEPS + 2;
gtab(i) = rdtable(TN, ba.time : gen, i)
with {
    gen(k) = w / (1 + w)
    with {
        fc = min(20 * pow(2, k / STEPS), 0.45 * ma.SR);
        w  = tan(ma.PI * fc / ma.SR);
    };
};
vcf_g   = g0 + (x - i) * (gtab(i + 1) - g0)
with {
    x  = vcf_oct * STEPS;
    i  = int(x);
    g0 = gtab(i);
};
// self-oscillates above k = 4 (resonance ~0.93): playable on its own with
// the oscillators off (Kybd 1 tracks the keys); the saturation bounds it
vcf_k   = 4.3 * resonance;

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
mono = dco * 0.25 + thermal + kick : ladder(vcf_g, vcf_k, bass_comp) : *(vca * gain * 2 * level) : attach(_, patch);

// pan by voice: every voice has its own place in the stereo field (the
// polyphony numbers them, [poly:voice]), neighbouring voices far apart
// (golden ratio steps from the centre); notes move through the voices, so
// even the same note wanders. Spread 0 = centre. In FAUST's own polyphony
// every voice is number 0: the centre.
voice_no = hslider("voice [poly:voice] [hidden:1]", 0, 0, 15, 1);
place    = ma.frac(voice_no * 0.618034 + 0.5) - 0.5;    // -0.5 .. 0.5
angle    = (0.5 + place * spread) * ma.PI / 2;
voice    = mono <: *(cos(angle) * sqrt(2)), *(sin(angle) * sqrt(2));   // centre: the old level

// chorus after the voice mix (the 'effect' runs once, not per voice):
// two modulated delays in opposite phase, rates and depths after the Juno 60
chorus_rate  = ba.selectn(4, chorus, 0, 0.5, 0.83, 8);
chorus_depth = ba.selectn(4, chorus, 0, 0.0018, 0.0018, 0.0003);
chorus_lfo   = os.lf_triangle(chorus_rate);
// the BBD's output filter: 10 kHz, 12 dB/oct, on the chorus delays only
tap(sign)    = de.fdelay(1024, ma.SR * (0.0035 + sign * chorus_depth * chorus_lfo)) : fi.lowpass(2, 10000);
chorused(lv, rv) = l * dry + tap(1, l) * wet, r * dry + tap(-1, r) * wet
with {
    hpf(x) = x : fi.highpass(1, hpf_freq) : fi.low_shelf1_l(hpf_boost, 120);   // the HPF after the voices
    l = lv : hpf;
    r = rv : hpf;
    wet = ba.if(chorus > 0, 0.7, 0);
    dry = ba.if(chorus > 0, 0.7, 1);
};

stereo = chorused;   // (a limiter here sounded bad: lower Level when chords clip)

process = voice;
effect = stereo;
