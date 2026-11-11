declare name "strike";
declare description "Six voice physical modelling percussion: mallet and noise exciters into modal resonators (string, beam, marimba, drumhead, membrane, plate); triggers on IN 1-6 and MIDI (a channel per voice, or notes 36-41)";
declare author "Arev Imer";
declare version "0.6";
declare block_size "64";            // 2.7 ms round trip; per block work (the modes from the settings) halves against 32
declare audio_outputs "1-8";
declare options "[itcm:on]";   // the loop of six voices (~35 kB) outgrew the 32 kB instruction cache: it runs from ITCM

// Six drum voices, each an exciter striking a resonating object:
//
//   - exciter: a mallet (an impulse, its stiffness a low-pass: soft and
//     round .. hard and bright) and noise (attack/decay envelope, density
//     from single grains to white noise, low- and high-pass), mixed
//   - object: 6 modes (two-pole resonators) with the partial ratios of a
//     string, beam, marimba bar, drumhead (circular membrane), membrane
//     (rectangular) or plate. Decay, strike position (edge .. middle:
//     which partials sound), tone (material: high partials decay faster
//     for wood, as long as the low ones for metal), inharmonicity (partials
//     closer together or spread), low cut, and a pitch envelope (bend)
//   - noise out: the noise also straight to the output (snare wires, hats)
//   - choke groups A/B: a voice silences the others of its group (hats)
//
// Each voice's parameters are settings: "voice" selects which voice the
// settings list shows and POT A-D edit (inharmonic, position, bend, tension). A knob
// takes over when it is turned (jump), the settings are saved per voice.
//
// in 1-6: triggers of voices 1-6 (rising edge above ~1 V)
// MIDI (setting "midi"): channels: voice 1-6 on channels base..base+5 (the
//         MIDI channel system setting, omni = 1), any note, C4 = the
//         voice's pitch (the last note also sets the pitch for input
//         triggers), with velocity; notes: notes 36-41 (C1-F1) on the
//         base channel
// out 1/2: stereo mix (soft clipped at 4.5 V)   out 3-8: voices 1-6 (mono)
//
// hardware controls (rev2)                      rev1
// X Y Z (knob + CV): assignable, for all voices (settings x, y, z: decay,
//   hardness, drive, pitch, tone, noise, position, bend, tension, variation,
//   rattle; default decay / hardness / drive)      rev1: CV X Y Z
// POT A-D: inharmonic, position, bend, tension of the selected voice
// button 1: next voice (on release); held while turning POT A-D: all six
//   voices move by the same amount; held while pressing button 2: a new
//   random sound for the selected voice (played); button 2: plays the
//   selected voice
// LEDs: 1 the selected voice plays, 2 any voice; the home screen shows the
//   selected voice, its type and a strip of the six voices
// settings list: the selected voice's settings in sections object (type,
//   pitch, decay, position, tone, inharmonic, low cut, bend, bend time),
//   exciter (noise blend, stiffness, noise attack, noise decay, density, noise lp,
//   noise hp), output (noise out, level, pan, choke), then kit (midi)

import("stdfaust.lib");
import("nemesis.lib");

NV = 6;          // voices
NM = 6;          // most modes of a voice
// modes per voice slot: the kick, tom and rim slots need fewer partials than
// the hats (inharmonic plates). The code of 6 x 6 modes (40 kB) outgrew the
// Teensy's 32 kB instruction cache, the program runs from flash: ~85 % CPU
nm(v) = ba.take(v, (4, 5, 4, 6, 6, 4));
NB = 2;          // modes that follow the pitch envelope (bend)

// ---- global controls -----------------------------------------------------

// the voice the settings list shows and POT A-D edit: button 1 steps it
// (the home screen shows it with its type), button 2 plays it
voice_sel  = hslider("voice [pager:1] [pagename:v{voice} type]", 1, 1, NV, 1);
audition   = button("audition [button:2]");
// X / Y / Z (knob + CV): what they control for all voices is a setting each
// (kit section: x, y, z); the popup is named after it. The centre is neutral
// except for drive, variation and rattle (left: nothing added).
x_target   = hslider("[31] x [section:kit] [style:menu{'decay all':0;'hardness':1;'drive':2;'pitch all':3;'tone all':4;'noise all':5;'position all':6;'bend all':7;'tension all':8;'variation all':9;'rattle all':10}]", 0, 0, 10, 1) : int;
y_target   = hslider("[32] y [section:kit] [style:menu{'decay all':0;'hardness':1;'drive':2;'pitch all':3;'tone all':4;'noise all':5;'position all':6;'bend all':7;'tension all':8;'variation all':9;'rattle all':10}]", 1, 0, 10, 1) : int;
z_target   = hslider("[33] z [section:kit] [style:menu{'decay all':0;'hardness':1;'drive':2;'pitch all':3;'tone all':4;'noise all':5;'position all':6;'bend all':7;'tension all':8;'variation all':9;'rattle all':10}]", 2, 0, 10, 1) : int;
knob_x     = hslider("X [hw:CV_X] [title:x]", 0.5, 0, 1, 0.001);
knob_y     = hslider("Y [hw:CV_Y] [title:y]", 0.5, 0, 1, 0.001);
knob_z     = hslider("Z [hw:CV_Z] [title:z]", 0, 0, 1, 0.001);
// the knob assigned to destination d (X before Y before Z), else neutral
macro(d, neutral) = select2(x_target == d, select2(y_target == d, select2(z_target == d, neutral, knob_z), knob_y), knob_x);

dec_scale  = 2 ^ ((macro(0, 0.5) - 0.5) * 6);           // 1/8 .. 1 .. 8 x the decays
stiff_off  = (macro(1, 0.5) - 0.5) * 0.8;               // mallet stiffness offset
drive      = macro(2, 0);
pitch_all  = (macro(3, 0.5) - 0.5) * 24;                // +-12 semitones
tone_off   = macro(4, 0.5) - 0.5;
noise_off  = macro(5, 0.5) - 0.5;
pos_off    = macro(6, 0.5) - 0.5;
bend_scale = macro(7, 0.5) * 2;                         // 0 .. 2 x the bends
tens_scale = macro(8, 0.5) * 2;
var_off    = macro(9, 0);
rattle_off = macro(10, 0);

// POT A-D: edit the selected voice (settings with the most effect on the sound)
edit_a     = hslider("inharmonic [rev2:POT_A] [edit:v{voice} inharmonic]", 0.5, 0, 1, 0.001);
edit_b     = hslider("position [rev2:POT_B] [edit:v{voice} position]", 0.5, 0, 1, 0.001);
edit_c     = hslider("bend [rev2:POT_C] [edit:v{voice} bend]", 0.5, 0, 1, 0.001);
edit_d     = hslider("tension [rev2:POT_D] [edit:v{voice} tension]", 0.5, 0, 1, 0.001);

// ---- partial ratios of the objects ------------------------------------------

ratio(type, k) = ba.selectn(6, type,
    ba.take(k + 1, (1, 2, 3, 4, 5, 6, 7, 8)),                                       // string
    ba.take(k + 1, (1, 2.756, 5.404, 8.933, 13.345, 18.638, 24.814, 31.87)),          // beam (free bar)
    ba.take(k + 1, (1, 3.9, 9.9, 18.4, 29.1, 41.8, 56.7, 73.6)),                      // marimba (tuned bar)
    ba.take(k + 1, (1, 1.594, 2.136, 2.296, 2.653, 2.918, 3.156, 3.501)),             // drumhead (Bessel)
    ba.take(k + 1, (1, 1.581, 2, 2.236, 2.55, 2.915, 3, 3.162)),                      // membrane (square)
    ba.take(k + 1, (1, 2.5, 4, 5, 6.5, 8.5, 9, 10)));                                 // plate

// sin(s) and cos(s) for 0 <= s <= pi/2 (polynomials: no libm per sample)
sin_p(s) = s * (1 - s2 / 6 * (1 - s2 / 20 * (1 - s2 / 42))) with { s2 = s * s; };
cos_p(s) = 1 - s2 / 2 * (1 - s2 / 12 * (1 - s2 / 30 * (1 - s2 / 56))) with { s2 = s * s; };

// one mode: a two-pole resonator at f (per sample: the pitch envelope),
// radius r for its decay; the input gain sin(w) makes an impulse ring with
// amplitude amp whatever the frequency. Noise builds up with the decay
// (variance / (2 (1 - r^2))): it comes in scaled by sqrt(2 (1 - r^2)), so a
// noise burst of a given level rings as loud as a mallet of that level.
// cos(w) = 1 - 2 sin^2(w/2) keeps low frequencies exact in float.
mode(f, r, amp, m, nz) = (m + nz * sqrt(2 * (1 - r * r))) * amp * sinw * audible : fi.tf2(1, 0, 0, -2 * r * cosw, r * r)
with {
    w       = 2 * ma.PI * f / ma.SR : min(0.95 * ma.PI);
    s       = w / 2;
    sn      = sin_p(s);
    cosw    = 1 - 2 * sn * sn;
    sinw    = 2 * sn * cos_p(s);
    audible = f < 0.45 * ma.SR;
};

// ---- a voice ------------------------------------------------------------------

// rising edge of a trigger input (hysteresis 0.1 .. 0.2 of full scale; IN
// 1-4 are AC coupled, an edge still comes through)
in_trig(x) = st & (1 - st')
with {
    st = ((x > 0.2) | (_ & (x > 0.1))) ~ _;
};

// per voice trigger (input edge or MIDI note 35 + v), velocity and choke
// group: functions of v, so the voices can see each other's (FAUST shares
// sliders with the same label)
// MIDI (the library's note parameters, nemesis_program.cpp): "channels":
// voice v on channel base + v - 1 (base: the MIDI channel system setting),
// any note, C4 (60) plays the voice's pitch, other notes transpose it;
// "notes": notes 36-41 on the base channel, one per voice
// randomize: confirm "voice" or "all" in the list; amount: how far
// random: target (the selected voice or all), roll (click, then every detent
// right a new roll, left back to the one before; #0 = before rolling), amount
// (100 % random .. small changes); button 1 held + button 2: a new roll of
// the selected voice, played at once
roll_target  = hslider("[26] target [section:random] [action:target] [style:menu{'voice':0;'all':1}]", 0, 0, 1, 1);
roll_pos     = hslider("[27] roll [section:random] [action:roll]", 0, 0, 1000, 1);
random_amt   = hslider("[28] amount [section:random] [action:amount] [unit:%]", 100, 0, 100, 1);
midi_mode    = hslider("[30] midi [section:kit] [style:menu{'channels':0;'notes 36-41':1}]", 0, 0, 1, 1) : int;
ch_count(v)  = hslider("v%v ch trig [mono:trig] [chan:%v]", 0, 0, 1000000, 1);
ch_note(v)   = hslider("v%v ch note [mono:note] [chan:%v]", 60, 0, 127, 1);
ch_vel(v)    = hslider("v%v ch velocity [mono:velocity] [chan:%v]", 1, 0, 1, 0.001);
key_count(v) = hslider("v%v key trig [mono:trig] [note:%k]", 0, 0, 1000000, 1) with { k = 35 + v; };
key_vel(v)   = hslider("v%v key velocity [mono:velocity] [note:%k]", 1, 0, 1, 0.001) with { k = 35 + v; };
changed(x)   = x != x';
midi_trig(v) = select2(midi_mode, changed(ch_count(v)), changed(key_count(v)));
midi_vel(v)  = select2(midi_mode, ch_vel(v), key_vel(v));
audition_trig(v) = (audition > audition') & (voice_sel == v);
trig_of(v, ins) = in_trig(ba.take(v, ins)) | midi_trig(v) | audition_trig(v);
choke_of(v)  = hslider("[24] v%v choke [name:choke] [random:0] [section:output] [if:voice=%v] [style:menu{'off':0;'A':1;'B':2}]", ba.take(v, (0, 0, 0, 1, 1, 0)), 0, 2, 1) : int;
pan_of(v)    = hslider("[23] v%v pan [name:pan] [random:0] [section:output] [if:voice=%v]", ba.take(v, (0, 0, -0.3, 0.3, 0.35, -0.4)), -1, 1, 0.01);
// another voice of the same group (A or B) started
choked(v, ins) = sum(j, NV, ((j + 1) != v) * trig_of(j + 1, ins) * (choke_of(j + 1) == choke_of(v))) * (choke_of(v) > 0) > 0;

voice(v, ins) = out * vca * level
with {
    // per voice settings, listed while "voice" is v
    type     = hslider("[01] v%v type [name:type] [section:object] [if:voice=%v] [style:menu{'string':0;'beam':1;'marimba':2;'drumhead':3;'membrane':4;'plate':5}]",
                       ba.take(v, (3, 4, 3, 5, 5, 1)), 0, 5, 1) : int;
    pitch    = hslider("[02] v%v pitch [name:pitch] [section:object] [if:voice=%v] [unit:st]", ba.take(v, (31, 50, 43, 96, 96, 79)), 12, 108, 1);
    decay    = hslider("[03] v%v decay [name:decay] [section:object] [if:voice=%v] [scale:log] [unit:s]", ba.take(v, (0.5, 0.25, 0.6, 0.08, 0.7, 0.15)), 0.02, 8, 0.01);
    pos_set = hslider("[04] v%v position [name:position] [section:object] [if:voice=%v]", ba.take(v, (0.2, 0.3, 0.25, 0.4, 0.4, 0.3)), 0, 1, 0.01);
    tone_set = hslider("[05] v%v tone [name:tone] [section:object] [if:voice=%v]", ba.take(v, (0.3, 0.4, 0.35, 0.9, 0.9, 0.6)), 0, 1, 0.01);
    inharm   = hslider("[06] v%v inharmonic [name:inharmonic] [section:object] [if:voice=%v]", ba.take(v, (0, 0, 0, 0.6, 0.6, 0)), -1, 1, 0.01);
    lowcut   = hslider("[07] v%v low cut [name:low cut] [section:object] [if:voice=%v] [scale:log] [unit:Hz]", ba.take(v, (20, 80, 20, 3000, 3000, 20)), 20, 4000, 1);
    bend_set = hslider("[08] v%v bend [name:bend] [section:object] [if:voice=%v] [unit:st]", ba.take(v, (12, 3, 5, 0, 0, 0)), -24, 24, 0.1);
    bend_t   = hslider("[09] v%v bend time [name:bend time] [section:object] [if:voice=%v] [scale:log] [unit:ms]", ba.take(v, (40, 20, 80, 20, 20, 20)), 2, 500, 1);
    tens_set = hslider("[10] v%v tension [name:tension] [section:object] [if:voice=%v] [unit:st]", ba.take(v, (3, 1, 4, 0, 0, 0)), 0, 12, 0.1);
    dynamics = hslider("[13] v%v dynamics [name:dynamics] [section:exciter] [if:voice=%v]", 0.5, 0, 1, 0.01);
    var_set = hslider("[14] v%v variation [name:variation] [section:exciter] [if:voice=%v]", 0.15, 0, 1, 0.01);
    rattle_set = hslider("[21] v%v rattle [name:rattle] [section:output] [if:voice=%v]", ba.take(v, (0, 0.5, 0, 0, 0, 0)), 0, 1, 0.01);
    mix_set  = hslider("[11] v%v noise blend [name:noise blend] [section:exciter] [if:voice=%v]", ba.take(v, (0.1, 0.4, 0.1, 0.8, 0.8, 0)), 0, 1, 0.01);  // mallet .. noise
    stiff    = hslider("[12] v%v stiffness [name:stiffness] [section:exciter] [if:voice=%v]", ba.take(v, (0.3, 0.7, 0.5, 0.9, 0.9, 0.9)), 0, 1, 0.01);
    n_att    = hslider("[15] v%v noise attack [name:noise attack] [section:exciter] [if:voice=%v] [scale:log] [unit:ms]", ba.take(v, (1, 0.5, 1, 0.2, 0.2, 0.5)), 0.1, 200, 0.1);
    n_dec    = hslider("[16] v%v noise decay [name:noise decay] [section:exciter] [if:voice=%v] [scale:log] [unit:ms]", ba.take(v, (60, 300, 80, 80, 700, 30)), 2, 4000, 1);
    density  = hslider("[17] v%v density [name:density] [section:exciter] [if:voice=%v]", 1, 0, 1, 0.01);
    n_lp     = hslider("[18] v%v noise lp [name:noise lp] [section:exciter] [if:voice=%v] [scale:log] [unit:Hz]", ba.take(v, (3000, 9000, 4000, 16000, 16000, 8000)), 100, 18000, 1);
    n_hp     = hslider("[19] v%v noise hp [name:noise hp] [section:exciter] [if:voice=%v] [scale:log] [unit:Hz]", ba.take(v, (20, 800, 20, 6000, 6000, 200)), 20, 12000, 1);
    n_out    = hslider("[20] v%v noise out [name:noise out] [section:output] [if:voice=%v]", ba.take(v, (0, 0.5, 0, 0.6, 0.6, 0)), 0, 1, 0.01);
    level    = hslider("[22] v%v level [name:level] [random:0] [section:output] [if:voice=%v]", ba.take(v, (0.9, 0.8, 0.8, 0.6, 0.6, 0.8)), 0, 1, 0.01) : si.smoo;
    choke    = choke_of(v);
    trig     = trig_of(v, ins);
    vel_in   = select2(midi_trig(v), 1, midi_vel(v)) : ba.sAndH(trig);
    // the settings plus what X / Y / Z add for all voices
    lim01(x) = max(0, min(1, x));
    pos      = lim01(pos_set + pos_off);
    tone     = lim01(tone_set + tone_off);
    bend     = bend_set * bend_scale;
    tension  = tens_set * tens_scale;
    variation = lim01(var_set + var_off);
    rattle   = lim01(rattle_set + rattle_off);

    // variation: every hit a little different (held per hit): softer,
    // more or less bend and noise, the lowest modes slightly detuned
    // the voice's two noise sources and their previous samples, held
    rnd(k)   = ba.selectn(4, k, nz, dz, nz', dz') : ba.sAndH(trig);
    vel      = vel_in * (1 - variation * 0.4 * abs(rnd(0)));
    mix      = mix_set + noise_off + variation * 0.3 * rnd(1) : max(0) : min(1);   // noise blend: mallet .. noise
    bend_v   = bend * (1 + variation * 0.5 * rnd(2));
    detune   = 1 + variation * 0.03 * rnd(3);
    // channel mode: the last note transposes (C4 = the voice's pitch; also
    // for input triggers). The library writes notes at the start of a block,
    // so this stays control rate: a per sample pitch would put every mode's
    // coefficients on every sample (+23 % CPU)
    transpose = select2(midi_mode == 0, 0, ch_note(v) - 60);

    // pitch envelope: 1 at the strike, decaying with bend time; tension:
    // a loud hit raises the pitch, which relaxes as the drum decays (an
    // envelope of the velocity with the voice's decay time)
    pe_coef   = exp(-1 / (bend_t * 0.001 * ma.SR));
    penv      = (select2(trig, _ * pe_coef, 1)) ~ _;
    te_coef   = exp(-6.91 / (decay * dec_scale * ma.SR));
    tenv      = (select2(trig, _ * te_coef, vel)) ~ _;
    f0        = ba.midikey2hz(pitch + transpose + pitch_all);
    // 2^(semitones/12) as (1 + y/32)^32 (5 squarings, no pow per sample;
    // within 3 % at +-2 octaves)
    y         = (bend_v * penv + tension * tenv) / 12 * 0.6931472 / 32 + 1;
    f         = f0 * detune * (y : sq : sq : sq : sq : sq) with { sq(x) = x * x; };

    // exciter: mallet impulse through two one-pole low-passes (stiffness
    // 150 Hz .. 16 kHz, plus the hardness knob), noise burst
    st_fc     = 150 * (16000 / 150) ^ (stiff + stiff_off : max(0) : min(1));
    // dynamics: loud hits brighter, the soft mallet blended towards the
    // bare impulse by velocity
    impulse   = trig * vel;
    soft      = impulse : fi.lowpass(1, st_fc) : fi.lowpass(1, st_fc);
    bright    = dynamics * vel * vel;
    mallet    = (soft + bright * (impulse - soft)) * (1 - mix);
    t         = (+(1) : *(1 - trig) : min(10000000)) ~ _;
    att       = n_att * 0.001 * ma.SR;
    // linear attack, then an exponential decay (recursive: no exp per sample)
    n_coef    = exp(-6.91 / (n_dec * 0.001 * ma.SR * dec_scale));   // -60 dB after noise decay
    n_fall    = (select2(t < att, _ * n_coef, 1)) ~ _;
    nenv      = min(1, t / att) * n_fall * vel;
    nz        = no.noises(2 * NV, 2 * (v - 1));
    dz        = no.noises(2 * NV, 2 * (v - 1) + 1);
    d3        = density * density * density;
    grains    = nz * (abs(dz) < d3) / sqrt(max(d3, 0.01));
    noise     = select2(density > 0.98, grains, nz) * nenv : fi.lowpass(2, n_lp) : fi.highpass(2, n_hp);
    noise_in  = noise * mix;

    // the object: decay per mode from tone, amplitude from position, low cut
    inh       = 2 ^ inharm;
    rk(k)     = 1 + (ratio(type, k) - 1) * inh;
    t60(k)    = decay * dec_scale * rk(k) ^ ((tone - 1) * 1.5);
    radius(k) = exp(-6.91 / (t60(k) * ma.SR));
    amp(k)    = abs(sin((k + 1) * ma.PI * (0.03 + 0.47 * pos))) / (1 + 0.15 * k)
              * min(1, (f0 * rk(k) / lowcut) ^ 2);
    // the bend moves the lowest NB modes (their coefficients per sample),
    // the upper ones stay at the base pitch (control rate: half the CPU)
    fk(k)     = select2(k < NB, f0, f) * rk(k);
    object    = (mallet, noise_in) <: par(k, nm(v), mode(fk(k), radius(k), amp(k))) :> *(0.5);
    // rattle: wires on the drum, noise that follows its decay (the tension
    // envelope: a follower on the output cost more than all the rest new)
    wires     = nz : fi.highpass(1, 1500);
    buzz      = wires * tenv * rattle * 0.3;
    out       = object + noise * n_out + buzz;

    // choke: another voice of the group fades this one out (3 ms)
    vca       = select2(trig, select2(choked(v, ins), _, 0), 1) ~ _ : si.smooth(ba.tau2pole(0.003));
};

// ---- process ----------------------------------------------------------------

// IN 1-6 trigger the voices; out 1/2 the panned mix (drive, limiter), 3-8
// the voices
process(i1, i2, i3, i4, i5, i6) = (stereo : drive_st : par(c, 2, nm_softclip(0.9)) : meters, _), par(v, NV, vo(v + 1))
with {
    // activity per voice (150 ms after a trigger): the home screen strip,
    // LED 1 the selected voice, LED 2 any voice
    act(v)    = (+(1) : *(1 - trig_of(v, ins)) : min(100000)) ~ _ < 0.15 * ma.SR;
    // each bargraph attached on its own (a sum times 0 would be optimized away)
    // the edit knobs' own values are unused (the library writes the selected
    // voice's settings): attached, else FAUST drops them and POT A-D stay unbound
    meters    = attach(_, edit_a) : attach(_, edit_b) : attach(_, edit_c) : attach(_, edit_d)
              : seq(i, NV, attach(_, bar(i + 1)))
              : attach(_, roll_target) : attach(_, roll_pos) : attach(_, random_amt)
              : attach(_, sum(v, NV, act(v + 1) * (voice_sel == v + 1)) : hbargraph("selected [led:1]", 0, 1))
              : attach(_, sum(v, NV, act(v + 1)) > 0 : hbargraph("any [led:2]", 0, 1));
    bar(j)    = act(j) : hbargraph("v%j activity [activity:%j]", 0, 1);
    ins       = (i1, i2, i3, i4, i5, i6);
    vo(v)     = voice(v, ins);
    // pan gains from the raw setting (cos/sin once per block), smoothed after
    stereo    = par(v, NV, vo(v + 1) <: *(cos(p(v + 1)) : si.smoo), *(sin(p(v + 1)) : si.smoo)) :> _, _;
    p(v)      = (pan_of(v) + 1) * ma.PI / 4;
    g         = 1 + 9 * drive * drive : si.smoo;
    drive_st  = par(c, 2, \(x).(ma.tanh(x * g) / ma.tanh(g)));
};
