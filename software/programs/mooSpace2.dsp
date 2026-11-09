declare name "mooSpace2";
declare description "Moving spaces: mooSpace with wider controls and glide (pitch) or crossfade (no pitch) when the space moves";
declare author "Arev Imer";
declare version "2.8";
declare audio_outputs "1-6";   // 7/8 are envelope CVs
declare block_size "64";    // audio block (system setting block size: auto)

// mooSpace v1's algorithm (input diffusion, a figure eight tank with
// diffusers in the feedback, multi tap allpasses), with:
//
//   - wider control ranges; the knob centres sit where v1's defaults were
//     smear: how much the sound is blurred, the strength of all diffusers
//            (input and tank): left hardly any (clear, separate
//            reflections), centre v1, right dense (coefficients up to 0.9);
//            it also keeps the diffusers long enough to spread transients
//            (at least 2.5x the base lengths fully right), even at low drag
//     drag:  how far the space stretches in time, the lengths of the
//            diffusers (0.05x .. 9.5x, centre 4.75 as v1) and the late taps
//            (up to 2x v1): left tight and short, right long and dragging
//     modulate (setting, assignable to CV A-D): deeper, partly scaled with
//              the lengths, clockwise faster
//   - shimmer: a pitch shifter (setting shift, +12 by default) where the
//     input and the feedback enter each half of the tank: with a short
//     decay the input comes in shifted (an octave reverb), with a long one
//     every pass goes up again (the rising shimmer)
//   - tone, in the tank (its input and its feedback loops): the centre is
//     v1's default colour; to the left the damping in the loops grows and
//     the input darkens (down to 150 Hz), to the right high-passes in the
//     input and the loops thin the space (up to 2.5 kHz)
//   - the tank's level is held by soft limiters without time constants
//     (v1: slow compressors, which pumped at ~2 Hz with long decays and loud
//     signals), and each half has one more allpass (fixed, prime lengths):
//     more, closer resonances, so held tones beat less in the tail
//   - reverse: the signal going into the tank comes in reversed pieces of
//     a third of a second (two heads, crossfaded); the knob fades from
//     forward (0) to reverse echoes (1)
//   - sync (setting): the times lock to the transport's tempo (internal,
//     MIDI clock or IN 8): reverse pieces are a note value (reverse length),
//     lag a note value (the menu shows note values instead of ms), and the main late taps (drag) the
//     nearest multiple of a 1/32, so those echoes fall into the rhythm
//   - motion (setting): the delays in the tank have two read heads on one
//     buffer. on glides like v1 (pitch shifts while smear or modulate move
//     the space), off crossfades over 25 ms (no pitch, a grainy stepping).
//     The input diffusers too; lag always glides (CPU).
//
//   - freeze, echoes, duck, gate, lofi (settings), see their definitions
//
// in 1/2: stereo audio    out 1/2: stereo mix (limited to 4.5 V)
//   out 3/4: rear (wet, decorrelated)   out 5/6: wet only
//   out 7: tail envelope, out 8: input envelope (CV 0..5 V)
//
// hardware controls (rev2)                      rev1 (knob + CV)
// smear decay drag    X Y Z (knob + CV)          CV X Y Z
// reverse shimmer     POT A, POT B (+ CV A, B)   CV A, B
// tone mix            POT C, POT D (+ CV C, D)   CV C, F
// settings (turn the encoder): sync, reverse length, freeze, echoes, echo
//   time, duck, gate, lofi, modulate, shift, lag (ms, or a note value with
//   sync), push, motion
//   (rev1: lag CV D, push CV E)

import("stdfaust.lib");
import("nemesis.lib");

// ---- controls -------------------------------------------------------------

MAX_DIFF    = 9.5;    // drag: diffuser length multiplier as in v1: all diffusers fit in the fast RAM arena
MAX_LATE    = 10;     // drag: late tap multiplier (v1: 5)
MAX_LAG     = 2^16-2;    // lag up to 1 s / 1/2 note at 96 BPM (PSRAM)

// Computation order (faustdoc "Optimizing"): FAUST computes expressions of a
// raw slider once per block (control rate), but everything after si.smoo on
// every sample. So curves, pow, sin etc. come first, smoothing last (where
// it saves work: a linear curve after one shared smoother is just as cheap).
smear_knob  = hslider("smear [hw:CV_X]", 0.5, 0, 1, 0.001);
drag_knob   = hslider("drag [hw:CV_Z]", 0.5, 0, 1, 0.001);
// decay: the knob sets the tail time, even over the range (measured on the
// Mac: RT60 ~ 0.4 / -log10(gain)): 0.4 s .. 60 s exponential up to 0.95,
// then the gain goes past 1 to 1.05 (endless, held by the soft limiters);
// pow at control rate, smoothed last
decay_knob  = hslider("decay [hw:CV_Y]", 0.4, 0, 1., 0.001);
DECAY_TOP   = 0.95;
decay_rt    = 0.4 * 150 ^ (min(decay_knob, DECAY_TOP) / DECAY_TOP);
decay_g60   = 10 ^ (-0.4 / 60);
decay_g     = select2(decay_knob > DECAY_TOP, 10 ^ (-0.4 / decay_rt),
                decay_g60 + (1.05 - decay_g60) * (decay_knob - DECAY_TOP) / (1 - DECAY_TOP));

// freeze (setting, assignable to a CV as a gate): the input fades out of the
// tank, the loop gain goes to exactly 1 and damping and the tank high-pass
// open, so the tail holds unchanged (shimmer and modulate keep working on it)
freeze      = hslider("freeze [style:menu{'off':0;'on':1}]", 0, 0, 1, 1) > 0.5;
frozen      = freeze : si.smooth(ba.tau2pole(0.02));
// 1.004: the interpolating delay reads lose a little treble on every pass
// (frozen at exactly 1 it faded ~0.5 dB/s); the soft limiters hold the rest
decay       = decay_g * (1 - freeze) + 1.004 * freeze : si.smoo;
// tone and reverse coefficients are computed from the raw knob values: FAUST
// then evaluates their tan/sin once per block instead of on every sample
reverse_knob = hslider("reverse [rev1:CV_A] [rev2:POT_A] [cv:CV_A]", 0, 0, 1, 0.01);
shimmer_knob = hslider("shimmer [rev1:CV_B] [rev2:POT_B] [cv:CV_B]", 0, 0, 1, 0.001);
mod_knob    = hslider("modulate", 0.5, 0, 1, 0.001);   // a setting: assign it to a CV in the menu
tone_knob   = hslider("tone [rev1:CV_C] [rev2:POT_C] [cv:CV_C]", 0.5, 0, 1, 0.01);
rev_mix     = hslider("mix [rev1:CV_F] [rev2:POT_D] [cv:CV_D]", 0.5, 0, 1, 0.01);

// sync: times snap to the transport tempo (control rate: nm_bpm is a slider)
sync        = hslider("sync [style:menu{'off':0;'on':1}]", 0, 0, 1, 1) > 0.5;
sixteenth   = nm_samples(6) : max(64);                          // samples per 1/16 note
snap(len, grid) = select2(sync, len, floor(len / grid + 0.5) * grid);

// lag: in ms, or with sync a note value (two settings shown as "lag", the
// menu lists the one that applies)
lag_ms      = hslider("lag ms [name:lag] [ifnot:sync] [unit:ms] [rev1:CV_D]", 0, 0, 1000, 1);
lag_ticks   = hslider("lag sync [name:lag] [if:sync] [style:menu{'off':0;'1/32':1;'1/16':2;'1/8':3;'1/8.':4;'1/4':5;'1/4.':6;'1/2':7}]", 0, 0, 7.999, 1)
              : int : \(k).(ba.selectn(8, k, 0, 3, 6, 12, 18, 24, 36, 48));
predelay    = select2(sync, lag_ms * ma.SR / 1000, nm_samples(lag_ticks)) : min(MAX_LAG - 1) : si.smoo;
dist_amt    = hslider("push [rev1:CV_E]", 0, 0, 1, 0.01);
motion      = hslider("motion [style:menu{'off':0;'on':1}]", 1, 0, 1, 1) : si.smoo;   // on: glide (pitch), off: crossfade
xfade_ms    = 25;   // the crossfade's sweet spot (was a setting, 3..300 ms)

// knob curves: square laws, so the centre gives v1's default values
//   smear as v1 (centre 4.75); drag 0.5 -> 2.5 (v1 default), 1 -> 10
// drag: the diffuser lengths (v1's smear: centre 4.75, top 9.5), smoothed
// slowly (they glide) ...
stretch     = drag_knob * MAX_DIFF + 0.05 : si.smooth(0.998);
// ... and the late taps
late_raw    = 0.2 + drag_knob * drag_knob * (MAX_LATE - 0.2);   // never 0: the taps would coincide and cancel
// sync: the main late tap (left, 1800 x late_diff samples) on a 1/32 grid
late_synced = snap(1800 * late_raw, sixteenth / 2) / 1800 : max(0.2) : min(MAX_LATE);
late_diff   = late_synced : si.smoo;
// shimmer: a delay line pitch shifter (two heads SHIFT_W apart, crossfaded
// over SHIFT_X, as ef.transpose but with a buffer that fits in fast RAM) in
// the tank, where input and feedback enter it; the knob adds the shifted sound
SHIFT_W     = 3000;
SHIFT_X     = 750;
SHIFT_MAX   = 8192;
shift_semis = hslider("shift [style:menu{'+12':0;'+7':1;'+5':2;'+19':3;'-12':4}]", 0, 0, 4.999, 1)
              : int : \(k).(ba.selectn(5, k, 12, 7, 5, 19, -12));
shift_step  = 1 - 2 ^ (shift_semis / 12);   // read head speed, control rate
pitch_shift(x) = de.fdelay(SHIFT_MAX, d, x) * min(d / SHIFT_X, 1)
               + de.fdelay(SHIFT_MAX, d + SHIFT_W, x) * (1 - min(d / SHIFT_X, 1))
with {
    d = shift_step : (+ : +(SHIFT_W) : fmod(_, SHIFT_W)) ~ _;
};
shimmer_amt = shimmer_knob : si.smoo;
// the shifted sound is added (the original fades to half), so the shimmer
// builds up instead of draining the tail; the soft limiters in the tank
// catch the extra gain. Lowpassed, so the stacking octaves don't turn harsh.
shimmer(x)  = x * (1 - 0.5 * shimmer_amt) + (pitch_shift(x) : fi.lowpass(1, 7000)) * shimmer_amt;

// tone: -1 (dark) .. 0 (v1's default colour) .. 1 (thin)
tone        = tone_knob * 2 - 1;
dark        = max(0, -tone);
bright      = max(0, tone);
// damping in the loops: 0.1 at the centre (v1 default), 0.97 fully left
// (v1 went silent at 1), none fully right
damp        = select2(tone < 0, 0.1 * (1 - bright), 0.1 + 0.87 * sqrt(dark)) * (1 - freeze) : si.smoo;
// high-pass right of the centre: 20 Hz .. 2.5 kHz, in the loops and (two
// pole) at the tank input; low-pass at the input left of it: 18 kHz .. 150 Hz
hp_freq     = 20 * 125 ^ bright;
tank_hp     = fi.highpass(1, select2(freeze, hp_freq, 5));
// the dark side gets make-up gain (up to +18 dB), so a dark space stays audible
tone_gain   = 10 ^ (0.9 * dark) : si.smoo;
tank_tone   = fi.lowpass(1, 18000 * (150 / 18000) ^ dark) : fi.highpass(2, hp_freq) : *(tone_gain);

// reverse: pieces of REV_LEN samples read backwards (the read position
// moves back one sample per sample: delay 2 x phase), two heads half a piece
// apart with triangle windows that add up to 1
// piece length: a third of a second, or with sync a note value (at most
// REV_MAX: the buffer holds two pieces, 2^17 samples, PSRAM)
REV_MAX     = 65000;
rev_div     = hslider("reverse length [style:menu{'1/16':0;'1/8':1;'1/8.':2;'1/4':3;'1/2':4}]", 1, 0, 4.999, 1) : int
              : \(k).(ba.selectn(5, k, 1, 2, 3, 4, 8));      // in sixteenths
REV_LEN     = select2(sync, 16000, sixteenth * rev_div) : int : max(2048) : min(REV_MAX);
// free running, or with sync and a running transport on the beat grid
rev_free    = (+(1) : %(REV_LEN)) ~ _;
rev_beat    = int(ma.frac(nm_position / (6 * rev_div)) * REV_LEN) : min(REV_LEN - 1);
rev_phase   = select2(sync & (nm_running > 0), rev_free, rev_beat);
rev_tri(ph) = 1 - abs(2 * ph / REV_LEN - 1);
reversed(x) = rev_head(rev_phase) + rev_head((rev_phase + REV_LEN / 2) % REV_LEN)
with {
    rev_head(ph) = de.delay(2 * REV_MAX, 2 * ph, x) * rev_tri(ph);
};
// equal power from forward (0) to reversed (1), gains from the raw knob
rev_fwd     = cos(reverse_knob * ma.PI / 2) : si.smoo;
rev_back    = sin(reverse_knob * ma.PI / 2) : si.smoo;
reverse(x)  = x * rev_fwd + reversed(x) * rev_back;

// echoes: ping-pong taps on the reverse buffers, left at 1 and 3 note values,
// right at 2 and 4 (the transport's tempo, also when it is stopped), falling
// like a feedback delay, added to the signal going into the tank (smear
// blurs them); two reads per side (PSRAM)
echo_amt    = hslider("echoes", 0, 0, 1, 0.01) : si.smoo;
echo_ticks  = hslider("echo time [style:menu{'1/16':0;'1/8':1;'1/8.':2;'1/4':3;'1/4.':4}]", 1, 0, 4.999, 1) : int
              : \(k).(ba.selectn(5, k, 6, 12, 18, 24, 36));
ECHO_T      = nm_samples(echo_ticks) : int : max(1) : min((2 * REV_MAX - 1) / 4 : int);
echoes(side, x) = (de.delay(2 * REV_MAX, (1 + side) * ECHO_T, x) * ba.if(side, 0.36, 0.6)
                 + de.delay(2 * REV_MAX, (3 + side) * ECHO_T, x) * ba.if(side, 0.13, 0.22)) * echo_amt;
rev_echo(side, x) = reverse(x) + echoes(side, x);

// modulate: noon is off, both directions wobble all diffuser lengths, the
// clockwise side faster; relative to the lengths (works at any smear)
mod_amount  = mod_knob - 0.5 : *(2) : \(m).(m * m * m) : si.smoo;
mod_rate    = select2(mod_amount > 0, 0.29, 0.37 + 1.6 * mod_amount);
wobble      = abs(mod_amount) * os.oscs(mod_rate);   // no table: os.osc's is 256 kB
// relative (+-15 % of the lengths) plus an absolute part like v1 (works at smear 0)
// smear needs some length to spread a transient: the diffusers are at least
// smear_min long (0 .. 2.5x the base lengths, 4-20 ms), even at low drag
smear_min   = 2.5 * smear_knob * smear_knob : si.smooth(0.998);
diff_mult   = max(stretch, smear_min) * (1 + 0.15 * wobble) + 0.5 * wobble : max(0.05);

// diffusion: v1's coefficients from the smear centre up, less below it
// smear: the diffusers' strength, 0.2 .. 1 (v1) at the centre .. 1.2 (the
// coefficients are capped at 0.9, below that allpasses ring)
diffusion   = select2(smear_knob < 0.5, 1 + 0.4 * (smear_knob - 0.5), 0.2 + 1.6 * smear_knob) : si.smoo;

// ---- the moving delay -----------------------------------------------------

// two read heads with linear interpolation on one buffer, crossfaded by a
// triangle shared by all delays. motion sets how closely the heads follow
// the length: 1 both follow continuously and read the same position (a
// gliding delay: pitch while the space moves, like v1), 0 each head holds its
// length and takes a new one only while it is silent (crossfades, no pitch),
// in between they follow partly. One kind of head for both modes: running
// gliding and crossfading heads side by side overloads the Teensy.
xfade_ph    = (+(1 / (2 * max(16, xfade_ms * ma.SR / 1000))) : ma.frac) ~ _;
xfade_w     = 1 - abs(2 * xfade_ph - 1);                  // 0 .. 1 .. 0
// head A sounds with 1 - w: it is silent at w = 1 (phase 0.5), B at w = 0
// (the wrap); the triggers were the other way round, so at motion 0 every
// new length jumped in at full level (clicks instead of crossfades)
take_a      = (xfade_ph >= 0.5) & (xfade_ph' < 0.5);      // head A silent
take_b      = xfade_ph < xfade_ph';                       // head B silent

moving(maxd, len, x) = de.fdelay(maxd, da, x) * (1 - xfade_w) + de.fdelay(maxd, db, x) * xfade_w
with {
    d  = len : max(0) : min(maxd - 3);
    da = motion * d + (1 - motion) * ba.sAndH(take_a, d);
    db = motion * d + (1 - motion) * ba.sAndH(take_b, d);
};

// ---- v1's building blocks ---------------------------------------------------

i_diff_1 = 0.75 * diffusion : min(0.9);
i_diff_2 = 0.625 * diffusion : min(0.9);
d_diff_1 = 0.7 * diffusion : min(0.9);
d_diff_2 = 0.5 * diffusion : min(0.75);   // the late multi tap allpasses follow smear too

dt_idl_1	= 142;
dt_idl_2	= 107;
dt_idl_3	= 379;
dt_idl_4	= 277;
dt_idl_5	= 397;   // four more input diffusers for smear: fixed integer
dt_idl_6	= 491;   // lengths, primes (two gliding ones cost 9 % CPU)
dt_idl_7	= 607;
dt_idl_8	= 743;

dt_fbd_l1	= 151;
dt_fbd_l2	= 101;
dt_fbd_l3	= 367;
dt_fbd_l4	= 281;

dt_fbd_r1	= 149;
dt_fbd_r2	= 113;
dt_fbd_r3	= 397;
dt_fbd_r4	= 283;

dt_late_l1 = 672;
dt_late_l2 = 1800;
dt_late_r1 = 908;
dt_late_r2 = 2656;

rev_dt_1 = 4453;
rev_dt_2 = 3720;
rev_dt_3 = 4217;
rev_dt_4 = 3163;

swap = _,_ <: !,_,_,!;
clip(lo, hi) = min(hi) : max(lo);


// equal power crossfade; the sin gains come from the raw knob (control rate)
balance(mix, dry_l, wet_l, dry_r, wet_r) = pan(dry_l, wet_l), pan(dry_r, wet_r)
	with {
			wet_gain = sin(mix*ma.PI/2) : si.smoo;
			dry_gain = sin((1-mix)*ma.PI/2) : si.smoo;
			pan(dry, wet) = wet_gain * wet + dry_gain * dry;
		  };

// Schroeder allpass around a moving delay: tank and input diffusers (the
// input ones glided like v1 and bent the pitch at motion 0 when drag or
// smear changed their lengths; the CPU for that came from replacing the
// input's 1176 limiter + tanh with a soft clip)
allpass(dmax, dt, coeff) =
(+ <: moving(dmax, max(0, dt-1)), *(coeff)) ~ *(-coeff) : mem,_ : + ;
allpass_glide(dmax, dt, coeff) = allpass(dmax, dt, coeff);

damping(x) = _ * (1 - x) : + ~ (mem * x);

// soft limiting in the tank (instead of v1's compressors, attack 0.1..0.5 s,
// which pumped): linear below 90 % of the ceiling, then a smooth curve to it
tank_limit(ceiling) = nm_softclip(ceiling);

// an extra fixed allpass in each tank half (prime lengths, integer delay)
allpass_fixed(n, coeff) = (+ <: de.delay(n, n - 1), *(coeff)) ~ *(-coeff) : mem, _ : +;
TANK_AP_L   = 1103;
TANK_AP_R   = 1213;

// the input's protection as v1 (the filters moved into the tank: tone)
tank_filter = fi.dcblocker : nm_softclip(0.9);   // was a 1176 limiter + tanh (log/exp per sample)

DIFF_MAX(dt) = MAX_DIFF * dt + 4;   // modulation above that is clamped (buffers stay <= 16 kB)

// coefficients by drag as in v1, capped (drag goes to 2x v1)
// input diffusion by smear too (v1's 0.375 at the centre, up to 0.6): an
// allpass passes -coeff of its input at once, so high coefficients let the
// click through (4 x 0.85 kept 52 % of it); six stages at <= 0.6 fade the
// transient in instead (first 10 ms -19 dB at smear 1, was -2 dB)
in_coeff(c) = min(0.6, c * (0.2 + 0.6 * smear_knob + 0.6 * max(0, smear_knob - 0.5)));
input_diff =
	allpass_glide(DIFF_MAX(dt_idl_1), diff_mult * dt_idl_1, in_coeff(i_diff_1)) :
	allpass_glide(DIFF_MAX(dt_idl_2), diff_mult * dt_idl_2, in_coeff(i_diff_1)) :
	allpass_glide(DIFF_MAX(dt_idl_3), diff_mult * dt_idl_3, in_coeff(i_diff_2)) :
	allpass_glide(DIFF_MAX(dt_idl_4), diff_mult * dt_idl_4, in_coeff(i_diff_2)) :
	allpass_fixed(dt_idl_5, in_coeff(i_diff_2)) :
	allpass_fixed(dt_idl_6, in_coeff(i_diff_2)) :
	allpass_fixed(dt_idl_7, in_coeff(i_diff_2)) :
	allpass_fixed(dt_idl_8, in_coeff(i_diff_2)) :
	_;

feedback_diff_l =
	allpass(DIFF_MAX(dt_fbd_l1), diff_mult * dt_fbd_l1, i_diff_1) :
	allpass(DIFF_MAX(dt_fbd_l2), diff_mult * dt_fbd_l2, i_diff_1) :
	allpass(DIFF_MAX(dt_fbd_l3), diff_mult * dt_fbd_l3, i_diff_2) :
	allpass(DIFF_MAX(dt_fbd_l4), diff_mult * dt_fbd_l4, i_diff_2) :
	_;

feedback_diff_r =
	allpass(DIFF_MAX(dt_fbd_r1), diff_mult * dt_fbd_r1, i_diff_1) :
	allpass(DIFF_MAX(dt_fbd_r2), diff_mult * dt_fbd_r2, i_diff_1) :
	allpass(DIFF_MAX(dt_fbd_r3), diff_mult * dt_fbd_r3, i_diff_2) :
	allpass(DIFF_MAX(dt_fbd_r4), diff_mult * dt_fbd_r4, i_diff_2) :
	_;

// late multi tap allpasses (v1), lengths by drag
multi_tap_allpass_l(dt, coeff) =
	(+ <: de.sdelay(MAX_LATE * dt, 1024, late_diff * dt - 1), * (coeff), de.sdelay(MAX_LATE * dt, 1024, late_diff*(dt/9.63)), de.sdelay(MAX_LATE * dt, 1024, late_diff*(dt/1.46)))
	~(* (-coeff), !, !, !) : mem, _, _, _ : +(_, _), _, _;

multi_tap_allpass_r(dt, coeff) =
	(+ <: de.sdelay(MAX_LATE * dt, 1024, late_diff * dt - 1), * (coeff), de.sdelay(MAX_LATE * dt, 1024, late_diff*(dt/7.93)), de.sdelay(MAX_LATE * dt, 1024, late_diff*(dt/1.39)))
	~(* (-coeff), !, !, !) : mem, _, _, _ : +(_, _), _, _;

// tank delays and taps as in v1 (moving them too costs more CPU than the
// Teensy has); the sdelay taps follow drag like in v1
tank_delay(dt) = de.delay(dt, dt);
tank_tap(dt, ratio) = de.fdelay(dt, dt / ratio);
tank_sdtap(dt, ratio) = de.sdelay(dt, 1024, late_diff * dt / ratio : min(dt - 3));

left_side =
	_ * decay, _ : + : shimmer : tank_limit(0.7) :
	allpass(DIFF_MAX(dt_late_l1), diff_mult * dt_late_l1, -d_diff_1)

	<: tank_delay(rev_dt_1),
	tank_tap(rev_dt_1, 2.23),
	tank_sdtap(rev_dt_1, 12.61),
	tank_tap(rev_dt_1, 1.23) :

	(damping(damp) : tank_hp : allpass_fixed(TANK_AP_L, 0.5)), _, _+_ :

	decay * _, _, _ :

	(multi_tap_allpass_l(dt_late_l2, d_diff_2), _, _) <:
	tank_delay(rev_dt_2), _, _, _, _,
	tank_sdtap(rev_dt_2, 1.39), !, !, !, !,
	tank_tap(rev_dt_2, 3.49), !, !, !, ! :

	_ * decay, _, swap, _, _, _ :
	_, _, _, 0.6*((-1* _) + _ + _), _ :
	_, _, _, swap :
	_, 0.6*((-1* _) - _ - _), _;


right_side =
	_ * decay, _ : + : shimmer : tank_limit(0.7) :
	allpass(DIFF_MAX(dt_late_r1), diff_mult * dt_late_r1, -d_diff_1)

	<: tank_delay(rev_dt_3),
	tank_tap(rev_dt_3, 2.0),
	tank_sdtap(rev_dt_3, 15.85),
	tank_tap(rev_dt_3, 1.42) :

	(damping(damp) : tank_hp : allpass_fixed(TANK_AP_R, 0.5)), _, _ + _ :

	decay * _, _, _ :

	(multi_tap_allpass_r(dt_late_r2, d_diff_2), _, _) <:
	tank_delay(rev_dt_4), _, _, _, _,
	tank_sdtap(rev_dt_4, 26.14), !, !, !, !,
	tank_tap(rev_dt_4, 1.58), !, !, !, !:

	_ * decay, _, swap, _, _, _ :
	_, _, _, 0.6*((-1* _) + _ - _), _ :
	_, _, _, swap :
	_, 0.6*((-1* _) - _ + _), _ :
	_, swap;


// lo-fi (setting): sample rate and bit reduction in the tank's feedback,
// so every pass gets grainier (48 kHz/16 bit .. 4 kHz/6 bit); off is exact
lofi_amt    = hslider("lofi", 0, 0, 1, 0.01);
lofi_hold   = 1 + int(11 * lofi_amt * lofi_amt);
lofi_q      = 2 ^ (16 - 10 * lofi_amt);
lofi_trig   = ((+(1) <: select2(_ >= lofi_hold, _, 0)) ~ _) == 0;   // no modulo per sample
lofi_iq     = 1 / lofi_q;
// the hold's images (above its Nyquist) went back into the loop on every
// pass and made long tails grow: a one-pole low-pass at the reduced Nyquist
// after it, and a small loss with the amount (rounding noise)
lofi_lp     = fi.lowpass(1, 0.5 * ma.SR / lofi_hold : min(18000));
lofi_loss   = 1 - 0.03 * lofi_amt;
lofi(x)     = select2(lofi_amt > 0.005, x, ba.sAndH(lofi_trig, x) * lofi_q + 0.5 : floor : *(lofi_iq) : lofi_lp : *(lofi_loss));

// duck (setting): the wet sound goes down while the input plays and blooms
// when it stops; gate (setting): the wet sound is chopped on the transport's
// grid (open the first half of every step, while the transport runs)
duck_amt    = hslider("duck", 0, 0, 1, 0.01) : si.smoo;
gate_ticks  = hslider("gate [style:menu{'off':0;'1/16':1;'1/8':2;'1/4':3;'1/2':4}]", 0, 0, 4.999, 1) : int
              : \(k).(ba.selectn(5, k, 0, 6, 12, 24, 48));
gate_open   = select2((gate_ticks > 0) & (nm_running > 0), 1,
                ma.frac(nm_position * (1 / max(1, gate_ticks))) < 0.5)
            : si.smooth(ba.tau2pole(0.003));

// outputs: 1/2 the mix (limited), 3/4 rear: the differences of the tank's
// taps (decorrelated from 1/2: quad with 1/2 in front), 5/6 wet only,
// 7 the tail's envelope, 8 the input's envelope (CV 0..5 V)
out_stage(dl, la, ra, lb, rb, dr) =
      (balance(rev_mix, dl, wl, dr, wr) : nm_limiter2(0.9)),
      (la - ra) * g, (lb - rb) * g,   // 3-6 shaped by the output setting
      wl, wr,
      tail_env, min(1, 2 * in_env)
with {
    in_env   = (abs(dl) + abs(dr)) * 0.5 : an.amp_follower_ar(0.005, 0.25);
    g        = (1 - duck_amt * min(1, 4 * in_env)) * gate_open;
    wl       = (la + ra) * g;
    wr       = (lb + rb) * g;
    tail_env = abs(wl) + abs(wr) : an.amp_follower_ar(0.01, 0.1) : *(2) : min(1);
};

input_proc = par(i,2,
	de.fdelay(MAX_LAG, predelay) : rev_echo(i) : input_diff : tank_filter : tank_tone : *(1 - frozen));

distortion_stereo(distortion) = par(i,2,cubic_distort(distortion, 0))
	with {
		cubic_distort(drive, offset) = * (pregain) : + (offset) : clip(-1, 1) : cubic
			with {
    			pregain = pow(10.0, 2 * drive) : si.smoo;   // pow at control rate
    			clip(lo, hi) = min(hi) : max(lo);
    			cubic(x) = x - x*x*x / 3;
		};
	};

process = _ ,_ : distortion_stereo(dist_amt)

    <: // split L/R to dry and wet

	(_, // left ch dry

	(input_proc) //  left/right to 2 ch

	, _) : // right ch dry

	(_, // left ch dry

		(_, // 1st in left_side

			swap, // swap 3 & 2 that feedback goes to 1st in on right_side

			_ : // 2nd in right_side

		(left_side, right_side):

		_, _, swap, _, _ : _, swap, _, _, _) ~ (swap : feedback_diff_l, feedback_diff_r  : tank_limit(0.5), tank_limit(0.5) : lofi, lofi)),

	_ : // right ch dry

	_, !, !, _, swap, _, _ :

	out_stage;
