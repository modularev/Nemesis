declare name "pollen";
declare description "Live granular sampler: grains from a looping live memory (freeze holds it), position, size, pitch, density, spray, blur reverb; free or synced to the transport";
declare author "Arev Imer";
declare version "0.3";
declare block_size "64";        // audio block (system setting block size: auto)
declare sample_rate "96000";   // system setting sample rate: auto (FM, crush and grains alias less)
declare audio_outputs "1,2,7,8"; // 3-6 are trigger/CV outputs (left exact)

// A prototype granulator for live input only.
//
//   - live memory: a loop per channel (2/4/8/10 s). While recording it
//     always holds the last seconds of IN 1/2; freeze fades the input out
//     (50 ms) and the loop keeps repeating itself, so grains can wander
//     through it for as long as it stays frozen. A frozen loop is saved on
//     the SD card (/NEMESIS/pollen.WAV, 32 bit float) and is there again
//     when pollen starts with freeze on (after another program or power off)
//   - grains: 8 voices. Each grain takes its start, size, pitch, direction
//     and pan when it starts and keeps them (a snapshot of the controls);
//     window shapes bell/tukey/decay/ramp/square/triangle
//   - rate: density = overlapping grains (0.25 .. 7, the rate follows the
//     size); with sync and a running transport POT A picks a note value
//     (1 bar .. 1/32) and grains start on that grid
//   - position: CW reads the newest audio, CCW further back; scan moves the
//     start through the memory (while frozen: 0 holds still, 1 plays at
//     the original speed); spray scatters the start randomly
//   - blur: a small reverb after the grains, send and decay on one knob
//
// in 1/2: stereo audio
// out 1/2: mix (limited to 4.5 V)   out 7/8: wet only
// out 3: trigger per grain (5 ms, 5 V)   out 4: envelope of the grains
// out 5: random value per grain (0..5 V)  out 6: pitch of the latest grain
//        (1 V/oct around 0 V)
//
// hardware controls (rev2)                      rev1
// position size pitch   X Y Z (knob + CV)        CV X Y Z
// density spray         POT A, POT B (+ CV A, B) CV A, B
// blur mix              POT C, POT D (+ CV C, D) CV C, F
// settings: freeze, memory, sync, scan, reverse, pitch spray, quantize,
//   shape, pan spread, tone, blur input (the input into the blur, heard
//   also with mix fully dry)

import("stdfaust.lib");
import("nemesis.lib");

N    = 8;        // grain voices
MAXL = 2^20;     // live memory buffer per channel (PSRAM): 10.9 s at 96 kHz (2^19 cut 8/10 s to 5.5 s there)

// ---- controls (curves from the raw sliders: control rate) ------------------

pos_knob   = hslider("position [hw:CV_X]", 1, 0, 1, 0.001);
size_knob  = hslider("size [hw:CV_Y]", 0.5, 0, 1, 0.001);
pitch_knob = hslider("pitch [hw:CV_Z]", 0.5, 0, 1, 0.001);
dens_knob  = hslider("density [rev1:CV_A] [rev2:POT_A] [cv:CV_A]", 0.5, 0, 1, 0.001);
spray_knob = hslider("spray [rev1:CV_B] [rev2:POT_B] [cv:CV_B]", 0, 0, 1, 0.001);
blur_knob  = hslider("blur [rev1:CV_C] [rev2:POT_C] [cv:CV_C]", 0, 0, 1, 0.001);
mix_knob   = hslider("mix [rev1:CV_F] [rev2:POT_D] [cv:CV_D]", 0.5, 0, 1, 0.01);

freeze     = hslider("freeze [loop:freeze] [style:menu{'off':0;'on':1}]", 0, 0, 1, 1) > 0.5;
mem_sel    = hslider("memory [style:menu{'2 s':0;'4 s':1;'8 s':2;'10 s':3}]", 1, 0, 3.999, 1) : int;
sync       = hslider("sync [style:menu{'off':0;'on':1}]", 0, 0, 1, 1) > 0.5;
scan       = hslider("scan", 0, -1, 1, 0.01);
rev_prob   = hslider("reverse", 0, 0, 1, 0.01);               // share of reversed grains
pitch_rand = hslider("pitch spray [unit:st]", 0, 0, 12, 0.1);
quant      = hslider("quantize [style:menu{'off':0;'semitones':1;'octaves':2}]", 0, 0, 2.999, 1) : int;
shape      = hslider("shape [style:menu{'bell':0;'tukey':1;'decay':2;'ramp':3;'square':4;'triangle':5}]", 0, 0, 5.999, 1) : int;
pan_spread = hslider("pan spread", 0.5, 0, 1, 0.01);
tone       = hslider("tone", 0.5, 0, 1, 0.01);
blur_in    = hslider("blur input", 0, 0, 1, 0.01) : si.smoo;   // input into the blur, also when mix is dry

// ---- live memory ------------------------------------------------------------

// length in samples; held while frozen (a new length would cut the loop).
// A frozen loop is saved on the SD card and loaded when the program starts
// frozen (nemesis_loop.h): the library writes its length to "restored",
// which counts until freeze is turned off.
mem_len    = ba.selectn(4, mem_sel, 2, 4, 8, 10) * ma.SR : min(MAXL - 8) : int;
restored   = hslider("loop length [loop:restored]", 0, 0, MAXL, 1) : int;
first      = 1 - 1';                                   // 1 in the first sample
L          = select2(restored > 0, ba.sAndH((1 - freeze) | first, mem_len), restored);
// 0 recording .. 1 frozen, already there in the first sample (a restored
// loop must not fade in the input)
frozen     = freeze : si.smooth(select2(first, ba.tau2pole(0.05), 0));
// w(t) = in(t) while recording, w(t - L) while frozen: a loop of L samples
live(x)    = (_ * frozen + x * (1 - frozen)) ~ de.delay(MAXL, L - 1);

// ---- grain parameters (one shared signal, each voice holds it at its start)

S_raw      = ma.SR * 0.02 * 100 ^ size_knob;          // 20 ms .. 2 s
density    = 2 ^ (dens_knob * 5 - 2) : min(N - 1);    // overlapping grains
rate_free  = density * ma.SR / S_raw;                 // grains per second

sync_on    = sync & (nm_running > 0);
ticks      = ba.selectn(6, int(dens_knob * 5.999), 96, 48, 24, 12, 6, 3);
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
age_hi     = select2(reversed, L - 4 - S * max(0, 1 - ratio), L - 4 - S);
d_start    = wrapL((age : max(age_lo) : min(age_hi)) + since_frz);

// voice that takes the next grain (round robin)
voice_no   = (+(trig) : %(N)) ~ _;

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

voice(i, wl, wr) = de.fdelay(MAXL, d, wl) * env * gl, de.fdelay(MAXL, d, wr) * env * gr, env
with {
    tr   = trig & (voice_no == i);
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
    // forward: start + ratio * t; reversed: from the end backwards
    d    = select2(revh, dh + t * (1 - rh), dh - rh * Sh + t * (1 + rh)) : max(0) : min(MAXL - 4);
    gl   = min(1, 1 - ph_);
    gr   = min(1, 1 + ph_);
};

// ---- tone and blur -------------------------------------------------------------

tt      = tone * 2 - 1;
tone_f  = fi.lowpass(2, 20000 * 0.01 ^ max(0, -tt)) : fi.highpass(2, 20 * 100 ^ max(0, tt));

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
process(inl, inr) = (inl * dry_g + gl * wet_g + bl, inr * dry_g + gr * wet_g + br : nm_limiter2(0.9)),
                    pulse, env_cv, rnd_cv, pitch_cv, wl, wr
with {
    ml      = live(inl);
    mr      = live(inr);
    grains  = par(i, N, voice(i, ml, mr)) :> _, _, _;
    gain    = 1.5 / sqrt(overlap) : si.smoo;
    gl      = grains : _, !, ! : *(gain) : tone_f;
    gr      = grains : !, _, ! : *(gain) : tone_f;
    env_sum = grains : !, !, _;
    bl      = blur(gl * wet_g + inl * blur_in, gr * wet_g + inr * blur_in) : _, !;
    br      = blur(gl * wet_g + inl * blur_in, gr * wet_g + inr * blur_in) : !, _;
    wl      = gl + bl;    // out 7/8: grains + blur (its grain part follows mix)
    wr      = gr + br;
    since   = (+(1) : *(1 - trig) : min(10000000)) ~ _;
    pulse   = since < 0.005 * ma.SR;
    env_cv  = env_sum / max(1, 0.6 * overlap) : min(1);
    rnd_cv  = ba.sAndH(trig, rn(5) * 0.5 + 0.5);
    pitch_cv = ba.sAndH(trig, semis) / 12 / 5         // 1 V/oct, 1.0 = 5 V
             : attach(_, L * freeze : hbargraph("loop [loop:length]", 0, MAXL));
};
