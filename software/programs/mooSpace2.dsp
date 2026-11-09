declare name "mooSpace2";
declare description "Moving spaces: mooSpace with wider controls and glide (pitch) or crossfade (no pitch) when the space moves";
declare author "Arev Imer";
declare version "2.1";
declare block_size "128";   // audio block (system setting block size: auto)

// mooSpace v1's algorithm (input diffusion, a figure eight tank with
// diffusers in the feedback, multi tap allpasses), with:
//
//   - wider control ranges; the knob centres sit where v1's defaults were
//     smear: same lengths as v1; below the centre the diffusion
//            coefficients drop too, so the echoes come apart (stutter)
//     drag:  late taps up to 2x v1: longer, more distinct echoes
//     modulate: deeper, partly scaled with the lengths, clockwise faster
//     colour: also darkens the wet input, so it works with a short decay
//   - motion (setting): the delays in the tank have two read heads on one
//     buffer. 1 glides like v1 (pitch shifts while smear or modulate move the
//     space), 0 crossfades (no pitch, a grainy stepping), in between follows
//     partly. The input diffusion always glides (CPU).
//
// in 1/2: stereo audio    out 1/2: stereo mix (limited to 4.5 V)
//
// hardware controls (rev2)                      rev1 (knob + CV)
// smear decay drag    X Y Z (knob + CV)          CV X Y Z
// filter modulate     POT A, POT B (+ CV A, B)   CV A, B
// colour mix          POT C, POT D (+ CV C, D)   CV C, F
// settings (turn the encoder): lag, push, motion, crossfade   (rev1: lag CV D, push CV E)

import("stdfaust.lib");
import("nemesis.lib");

// ---- controls -------------------------------------------------------------

MAX_DIFF    = 9.5;    // smear: diffuser length multiplier as in v1: all diffusers fit in the fast RAM arena
MAX_LATE    = 10;     // drag: late tap multiplier (v1: 5)
MAX_LAG     = 2^15-2;

// Computation order (faustdoc "Optimizing"): FAUST computes expressions of a
// raw slider once per block (control rate), but everything after si.smoo on
// every sample. So curves, pow, sin etc. come first, smoothing last (where
// it saves work: a linear curve after one shared smoother is just as cheap).
smear_knob  = hslider("smear [hw:CV_X]", 0.5, 0, 1, 0.001) : si.smooth(0.998);
drag_knob   = hslider("drag [hw:CV_Z]", 0.5, 0, 1, 0.001);
decay_raw   = hslider("decay [hw:CV_Y]", 0.3, 0, 1., 0.001) : sqrt;
decay       = decay_raw : si.smoo;
// filter and colour coefficients are computed from the raw knob values: FAUST
// then evaluates their tan/pow once per block instead of on every sample
filt_ctrl   = hslider("filter [rev1:CV_A] [rev2:POT_A] [cv:CV_A]", 0.5, 0, 1, 0.01);
mod_knob    = hslider("modulate [rev1:CV_B] [rev2:POT_B] [cv:CV_B]", 0.5, 0, 1, 0.001);
damp_knob   = hslider("colour [rev1:CV_C] [rev2:POT_C] [cv:CV_C]", 0.1, 0, 1, 0.01);
rev_mix     = hslider("mix [rev1:CV_F] [rev2:POT_D] [cv:CV_D]", 0.5, 0, 1, 0.01);

predelay    = hslider("lag [rev1:CV_D]", 0, 0, 1, 0.001) <: * : _ * (MAX_LAG - 1) : si.smoo;
dist_amt    = hslider("push [rev1:CV_E]", 0, 0, 1, 0.01);
motion      = hslider("motion", 1, 0, 1, 0.01) : si.smoo;   // 1 glide (pitch), 0 crossfade
xfade_ms    = hslider("crossfade [scale:log] [unit:ms]", 30, 3, 300, 1);

// knob curves: square laws, so the centre gives v1's default values
//   smear as v1 (centre 4.75); drag 0.5 -> 2.5 (v1 default), 1 -> 10
smear       = smear_knob * MAX_DIFF + 0.05;           // as v1: centre 4.75, top 9.5
late_raw    = 0.2 + drag_knob * drag_knob * (MAX_LATE - 0.2);   // never 0: the taps would coincide and cancel
late_diff   = late_raw : si.smoo;
damp        = damp_knob * 0.97 : si.smoo;             // v1 went silent at 1
// colour also darkens the wet input once (above v1's default 0.1), so it is
// heard with a short decay too: 18 kHz .. 1.2 kHz
pre_colour  = fi.lowpass(1, 18000 * (1200 / 18000) ^ (max(0, damp_knob - 0.1) / 0.9));

// modulate: noon is off, both directions wobble all diffuser lengths, the
// clockwise side faster; relative to the lengths (works at any smear)
mod_amount  = mod_knob - 0.5 : *(2) : \(m).(m * m * m) : si.smoo;
mod_rate    = select2(mod_amount > 0, 0.29, 0.37 + 1.6 * mod_amount);
wobble      = abs(mod_amount) * os.oscs(mod_rate);   // no table: os.osc's is 256 kB
// relative (+-15 % of the lengths) plus an absolute part like v1 (works at smear 0)
diff_mult   = smear * (1 + 0.15 * wobble) + 0.5 * wobble : max(0.05);

// diffusion: v1's coefficients from the smear centre up, less below it
diffusion   = min(1, 0.3 + 1.4 * smear_knob);

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
take_a      = xfade_ph < xfade_ph';                       // head A silent
take_b      = (xfade_ph >= 0.5) & (xfade_ph' < 0.5);      // head B silent

moving(maxd, len, x) = de.fdelay(maxd, da, x) * (1 - xfade_w) + de.fdelay(maxd, db, x) * xfade_w
with {
    d  = len : max(0) : min(maxd - 3);
    da = motion * d + (1 - motion) * ba.sAndH(take_a, d);
    db = motion * d + (1 - motion) * ba.sAndH(take_b, d);
};

// ---- v1's building blocks ---------------------------------------------------

i_diff_1 = 0.75 * diffusion;
i_diff_2 = 0.625 * diffusion;
d_diff_1 = 0.7 * diffusion;
d_diff_2 = 0.5;

dt_idl_1	= 142;
dt_idl_2	= 107;
dt_idl_3	= 379;
dt_idl_4	= 277;

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

lp_freq 	= filt_ctrl : clip(0.0, 0.48) * 2.083 <: * : _ * 15900 : _ + 100;
hp_freq		= filt_ctrl : (clip(0.52, 1) - 0.52) * 2.083 <: * : _ * 4980 : _ + 20;

// equal power crossfade; the sin gains come from the raw knob (control rate)
balance(mix, dry_l, wet_l, dry_r, wet_r) = pan(dry_l, wet_l), pan(dry_r, wet_r)
	with {
			wet_gain = sin(mix*ma.PI/2) : si.smoo;
			dry_gain = sin((1-mix)*ma.PI/2) : si.smoo;
			pan(dry, wet) = wet_gain * wet + dry_gain * dry;
		  };

// Schroeder allpass around a moving delay (in the tank), or a gliding one
// (input diffusion, as v1: moving delays cost too much CPU for all of them)
allpass(dmax, dt, coeff) =
(+ <: moving(dmax, max(0, dt-1)), *(coeff)) ~ *(-coeff) : mem,_ : + ;
allpass_glide(dmax, dt, coeff) =
(+ <: de.fdelay2(dmax, max(0, dt-1) : min(dmax - 3)), *(coeff)) ~ *(-coeff) : mem,_ : + ;

damping(x) = _ * (1 - x) : + ~ (mem * x);

filter_q = filt_ctrl : clip(0.4, 0.6) - 0.4 : _ * 10 - 1 : abs : _ * (3 - 2 * decay_raw) : max(0.1,_) ;
// make-up gain while the filter closes (up to ~ +10 dB at the lowpass end),
// so a filtered space stays audible
filter_makeup = (16000 / lp_freq) ^ 0.25 * (hp_freq / 20) ^ 0.08 : si.smoo;
tank_filter = fi.dcblocker : fi.resonlp(lp_freq, filter_q * 0.5, 1) : fi.resonhp(hp_freq, filter_q * 0.5, 1) : *(filter_makeup) : co.limiter_1176_R4_mono : ma.tanh;

DIFF_MAX(dt) = MAX_DIFF * dt + 4;   // modulation above that is clamped (buffers stay <= 16 kB)

// coefficients by drag as in v1, capped (drag goes to 2x v1)
in_coeff(c) = min(0.85, c * late_diff / 5);
input_diff =
	allpass_glide(DIFF_MAX(dt_idl_1), diff_mult * dt_idl_1, in_coeff(i_diff_1)) :
	allpass_glide(DIFF_MAX(dt_idl_2), diff_mult * dt_idl_2, in_coeff(i_diff_1)) :
	allpass_glide(DIFF_MAX(dt_idl_3), diff_mult * dt_idl_3, in_coeff(i_diff_2)) :
	allpass_glide(DIFF_MAX(dt_idl_4), diff_mult * dt_idl_4, in_coeff(i_diff_2)) :
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
	_ * (1.1*decay), _ : + : co.compressor_mono(8, -3, 0.5, 1) :
	allpass(DIFF_MAX(dt_late_l1), diff_mult * dt_late_l1, -d_diff_1)

	<: tank_delay(rev_dt_1),
	tank_tap(rev_dt_1, 2.23),
	tank_sdtap(rev_dt_1, 12.61),
	tank_tap(rev_dt_1, 1.23) :

	damping(damp), _, _+_ :

	decay * 1.1 * _, _, _ :

	(multi_tap_allpass_l(dt_late_l2, d_diff_2), _, _) <:
	tank_delay(rev_dt_2), _, _, _, _,
	tank_sdtap(rev_dt_2, 1.39), !, !, !, !,
	tank_tap(rev_dt_2, 3.49), !, !, !, ! :

	_ * decay  * 1.1, _, swap, _, _, _ :
	_, _, _, 0.6*((-1* _) + _ + _), _ :
	_, _, _, swap :
	_, 0.6*((-1* _) - _ - _), _;


right_side =
	_ * (1.1*decay), _ : + : co.compressor_mono(8, -3, 0.1, 1) :
	allpass(DIFF_MAX(dt_late_r1), diff_mult * dt_late_r1, -d_diff_1)

	<: tank_delay(rev_dt_3),
	tank_tap(rev_dt_3, 2.0),
	tank_sdtap(rev_dt_3, 15.85),
	tank_tap(rev_dt_3, 1.42) :

	damping(damp), _, _ + _ :

	decay * 1.1 * _, _, _ :

	(multi_tap_allpass_r(dt_late_r2, d_diff_2), _, _) <:
	tank_delay(rev_dt_4), _, _, _, _,
	tank_sdtap(rev_dt_4, 26.14), !, !, !, !,
	tank_tap(rev_dt_4, 1.58), !, !, !, !:

	_ * decay * 1.1, _, swap, _, _, _ :
	_, _, _, 0.6*((-1* _) + _ - _), _ :
	_, _, _, swap :
	_, 0.6*((-1* _) - _ + _), _ :
	_, swap;


input_proc = par(i,2,
	de.fdelay(MAX_LAG, predelay) : input_diff : tank_filter : pre_colour);

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

		_, _, swap, _, _ : _, swap, _, _, _) ~ (swap : feedback_diff_l, feedback_diff_r  : co.compressor_stereo(6, -6, 0.1, 0.5))),

	_ : // right ch dry

	_, !, !, _, swap, _, _ :

	_, (_ ,_ :> _), (_, _ :> _), _ : _, _, swap  : balance(rev_mix) : nm_limiter2(0.9);
