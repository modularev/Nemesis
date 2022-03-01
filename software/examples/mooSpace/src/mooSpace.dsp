declare name "mooSpace";
declare description "variable space reverberation audio effect";
declare author "Arev Imer (arev.imer@students.fhnw.ch)";
declare copyright "Arev";
declare version "0.2";

declare options "[midi:on]";

import("stdfaust.lib");

MAX_DIFF 	= 9.5;
MAX_LATE 	= 5;
MAX_MOD		= 150;
MAX_LAG		= 2^15-2;

diff_mult 	= hslider("smear[midi:ctrl 11]", 0.5, 0, 1, 0.001) : si.smoo: _ * MAX_DIFF;
late_diff 	= hslider("drag", 0.5, 0, 1, 0.001) : si.smoo: _ * MAX_LATE ;

mod			= hslider("modulate", 0.02, 0, 1, 0.001) : si.smoo * MAX_MOD * rand
	with {
		rand = (0.5 * (1 + no.lfnoiseN(3,5)));
		sine = (2*os.lf_sawpos(2))  * ma.PI : (sin + 1)*0.5;
	};

decay  		= hslider("decay", 0.3, 0, 1., 0.001) : sqrt : si.smoo;

predelay 	= hslider("lag", 0, 0, 1, 0.001) : si.smoo: _ * (MAX_LAG - 1);
damp 		= hslider("colour", 0.1, 0, 1, 0.01) : si.smoo;
ducktime 	= 0.2 * (dtime / 2);
dist_amt 	= hslider("push", 0, 0, 1, 0.01) : si.smoo;
filt_ctrl   = hslider("Filter", 0.5, 0, 1, 0.01) : si.smoo ;
diff_ctrl   = hslider("input diffusion", 0.5, 0, 1, 0.01) : si.smoo : _ * 1.3  ;
lp_freq 	= filt_ctrl : clip(0.0, 0.48) * 2.083 <: *       : _ * 15900 : _ + 100 ;
hp_freq		= filt_ctrl : (clip(0.52, 1) - 0.52) * 2.083  <: *: _ * 4980 : _ + 20 ;
rev_mix 	= hslider("mix", 0.5, 0, 1, 0.01) : si.smoo;
 

// Parameters

i_diff_1 = 0.75;
i_diff_2 = 0.625;

d_diff_1 = 0.7;
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


// Functions

swap = _,_ <: !,_,_,!;	

//clip(lower, upper, x) = min(upper,max(x,lower));
clip(lo, hi) = min(hi) : max(lo);

balance(mix, dry_l, wet_l, dry_r, wet_r) = sel(mix, dry_l, wet_l), sel(mix, dry_r, wet_r)
	with { 
			sel(mix, dry, wet) = (1-mix) * dry + mix * wet;
		  };


allpass(dmax, dt, coeff) = 
(+ <: de.fdelay(dmax, dt-1),*(coeff)) ~ *(-coeff) : mem,_ : + ;


bandwidth(x) = _ * x : + ~ (mem * (1 - x));
damping(x) = _ * (1 - x) : + ~ (mem * x);

filter_q = filt_ctrl : clip(0.4, 0.6) - 0.4 : _ * 10 - 1 : abs : _ * (3-2*decay) : max(0.1,_) ;
tank_filter = fi.resonlp(lp_freq, filter_q * 0.5, 1) : fi.resonhp(hp_freq, filter_q * 0.5, 1);


input_diff =
	allpass(MAX_DIFF * dt_idl_1, diff_mult * dt_idl_1+mod, i_diff_1 * diff_ctrl) :
	allpass(MAX_DIFF * dt_idl_2, diff_mult * dt_idl_2+mod, i_diff_1 * diff_ctrl) :
	allpass(MAX_DIFF * dt_idl_3, diff_mult * dt_idl_3+mod, i_diff_2 * diff_ctrl) :
	allpass(MAX_DIFF * dt_idl_4, diff_mult * dt_idl_4+mod, i_diff_2 * diff_ctrl) :
	_;

feedback_diff_l =
	allpass(MAX_DIFF * dt_fbd_l1, diff_mult * dt_fbd_l1+mod, i_diff_1) :
	allpass(MAX_DIFF * dt_fbd_l2, diff_mult * dt_fbd_l2+mod, i_diff_1) :
	allpass(MAX_DIFF * dt_fbd_l3, diff_mult * dt_fbd_l3+mod, i_diff_2) :
	allpass(MAX_DIFF * dt_fbd_l4, diff_mult * dt_fbd_l4+mod, i_diff_2) :
	_;

feedback_diff_r =
	allpass(MAX_DIFF * dt_fbd_r1, diff_mult * dt_fbd_r1+mod, i_diff_1) :
	allpass(MAX_DIFF * dt_fbd_r2, diff_mult * dt_fbd_r2+mod, i_diff_1) : 
	allpass(MAX_DIFF * dt_fbd_r3, diff_mult * dt_fbd_r3+mod, i_diff_2) :
	allpass(MAX_DIFF * dt_fbd_r4, diff_mult * dt_fbd_r4+mod, i_diff_2) :
	_;


multi_tap_allpass_l(dt, coeff) =

	(+ <: de.fdelay(MAX_LATE * dt + MAX_MOD, late_diff * dt - 1 + mod), * (coeff), de.fdelay(MAX_LATE * dt + MAX_MOD, late_diff*(dt/9.63)),de.fdelay(MAX_LATE * dt + MAX_MOD, late_diff*(dt/1.46)))
	~(* (-coeff), !, !, !) : mem, _, _, _ : +(_, _), _, _;


multi_tap_allpass_r(dt, coeff) =

	(+ <: de.fdelay(MAX_LATE * dt + MAX_MOD, late_diff * dt - 1 + mod), * (coeff), de.fdelay(MAX_LATE * dt + MAX_MOD, late_diff*(dt/7.93)),de.fdelay(MAX_LATE * dt + MAX_MOD, late_diff*(dt/1.39)))
	~(* (-coeff), !, !, !) : mem, _, _, _ : +(_, _), _, _;


left_side = 
	
	_ * (2*decay), _ : + : tank_filter : co.compressor_mono(8, -3, 0.5, 1) :
	allpass(MAX_LATE * dt_late_l1 + MAX_MOD, late_diff * dt_late_l1 + mod, -d_diff_1)

	<: de.fdelay(rev_dt_1, rev_dt_1),
	de.fdelay(rev_dt_1, (rev_dt_1/2.23)), // node24_30[1990] -> L-
	de.fdelay(rev_dt_1, late_diff * (rev_dt_1/12.61)), // node24_30[353] -> R+
	de.fdelay(rev_dt_1, (rev_dt_1/1.23)) : // node24_30[3627] -> R+

	damping(damp), _, _+_ :
	
	decay * 1.5 * _, _, _ :


	(multi_tap_allpass_l(dt_late_l2, d_diff_2), _, _) <: // node31_33[187] -> L-, node31_33[1228] -> R-
	de.fdelay(rev_dt_2, rev_dt_2), _, _, _, _,
	de.fdelay(rev_dt_2, (rev_dt_2/1.39)), !, !, !, !, // node33_39[2673] -> R+
	de.fdelay(rev_dt_2, (rev_dt_2/3.49)), !, !, !, ! : // node33_39[1066] -> L-
	
	_ * decay  * 1.5, _/*Lmult-*/,/*Rmult-*/ swap/*Lprev-*/, _/*Rsum+*/, _/*R+*/, _/*L-*/ :
	_, _, _, 0.6*((-1* _) + _ + _), _ :
	_, _, _, swap :
	_, 0.6*((-1* _) - _ - _), _; // 2nd GOING TO LEFT OUT / 3rd GOING TO RIGHT OUT


right_side = 

	_ * (2*decay), _ : + : tank_filter : co.compressor_mono(8, -3, 0.1, 1) :
	allpass(MAX_LATE * dt_late_r1 + MAX_MOD, late_diff * dt_late_r1 + mod, -d_diff_1)

	<: de.fdelay(rev_dt_3, rev_dt_3),
	de.fdelay(rev_dt_3, (rev_dt_3/2.0)), // node48_54[2111] -> R-
	de.fdelay(rev_dt_3, late_diff * (rev_dt_3/15.85)), // node48_54[266] -> L+
	de.fdelay(rev_dt_3, (rev_dt_3/1.42)) : // node48_54[2974]-> L+

	damping(damp), _, _ + _ :
	
	decay * 1.5 * _, _, _ :

	(multi_tap_allpass_r(dt_late_r2, d_diff_2), _, _) <: // node55_59[335] -> R-, node55_59[1913] -> L-
	de.fdelay(rev_dt_4, rev_dt_4), _, _, _, _,
	de.fdelay(rev_dt_4, late_diff * (rev_dt_4/26.14)), !, !, !, !, // node59_63[121] -> L-
	de.fdelay(rev_dt_4, (rev_dt_4 / 1.58)), !, !, !, !: // node59_63[1996] -> R+
	
	_ * decay * 1.5, _/*Rmult-*/,/*Lmult-*/swap/*Rprev-*/, _/*Lsum*/, _/*L-*/, _/*R+*/ :
	_, _, _, 0.6*((-1* _) + _ - _), _ :
	_, _, _, swap :
	_, 0.6*((-1* _) - _ + _), _ : // 2nd GOING TO RIGHT OUT / 3rd GOING TO LEFT OUT
	_, swap; // 2nd GOING TO LEFT OUT / 3rd GOING TO RIGHT OUT



input_proc = par(i,2,
	de.fdelay(MAX_LAG, predelay) : input_diff);

limiter = co.compressor_stereo(40,-1.5,0.0008,0.1);

distortion_stereo(distortion) = par(i,2,cubic_distort(distortion, 0))
	with {
		cubic_distort(drive, offset) = * (pregain) : + (offset) : clip(-1, 1) : cubic
			with {
    			pregain = pow(10.0, 2 * drive);
    			clip(lo, hi) = min(hi) : max(lo);
    			cubic(x) = x - x*x*x / 3;
    			postgain = max(1.0, 1.0 / pregain);
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

	_, (_ ,_ :> _), (_, _ :> _), _ : _, _, swap  : balance(rev_mix) :fi.dcblocker, fi.dcblocker :  limiter; 