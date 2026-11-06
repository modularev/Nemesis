declare name "freqshift";
declare description "Stereo frequency shifter (Bode style) with spread, feedback delay, sideband outputs and a quadrature oscillator";
declare author "Arev Imer";
declare version "0.2";
declare audio_outputs "1-6";   // the output setting leaves the oscillator on 7/8 exact

// A frequency shifter moves every partial of the input by the same number of
// Hz (unlike a pitch shifter, which multiplies them): small shifts give
// phasing and beating, large ones inharmonic, metallic sounds. Through the
// feedback delay every echo is shifted once more (spiralling echoes,
// barber pole effects).
//
// How: a pair of allpass chains splits the input into two signals 90 degrees
// apart (an analytic signal, "Hilbert transform"); multiplied with a sine
// and cosine at the shift frequency, their sum and difference are the input
// shifted up and down.
//
// in 1/2: stereo audio
// out 1/2: main L/R (dry/wet mix)
// out 3/4: shifted up L/R      out 5/6: shifted down L/R   (wet only, with feedback)
// out 7/8: shift oscillator of the left channel, sine and cosine, +-4.75 V:
//          a quadrature LFO at small shifts, an audio oscillator at large ones
//
// hardware controls (rev2)                       rev1
// shift, range, spread    X Y Z (knob + CV)       CV X Y Z
// mix, delay              POT A, B (+ CV A, B)    CV A, B
// feedback, sideband      POT C, D (+ CV C, D)    CV C, D
// damp (feedback lowpass) setting (turn the encoder)
// the home screen shows the shift of the left channel in Hz

import("stdfaust.lib");

// ---- controls -------------------------------------------------------------

// shift: bipolar, cubic, so the middle of the knob is fine for slow beating
shift_knob  = hslider("shift [hw:CV_X]", 0, -1, 1, 0.001) : si.smoo;
range       = hslider("range [hw:CV_Y] [scale:log] [unit:Hz]", 200, 10, 4000, 1) : si.smoo;
spread_knob = hslider("spread [hw:CV_Z]", 0, -1, 1, 0.001) : si.smoo;
mix         = hslider("mix [hw:POT_A] [cv:CV_A]", 1, 0, 1, 0.01) : si.smoo;
delay_ms    = hslider("delay [hw:POT_B] [cv:CV_B] [scale:log] [unit:ms]", 100, 1, 1000, 1) : si.smooth(0.9995);
feedback    = hslider("feedback [hw:POT_C] [cv:CV_C]", 0, 0, 1, 0.01) : si.smoo;
sideband    = hslider("sideband [hw:POT_D] [cv:CV_D]", 1, 0, 1, 0.01) : si.smoo;  // 0 down, 0.5 both, 1 up
damp        = hslider("damp", 0.3, 0, 1, 0.01) : si.smoo;

SPREAD_HZ   = 10;
shift_hz    = range * shift_knob ^ 3;
spread_hz   = SPREAD_HZ * spread_knob ^ 3;
shift_l     = shift_hz - spread_hz;
shift_r     = shift_hz + spread_hz;

// ---- analytic signal --------------------------------------------------------

// Olli Niemitalo's 90 degree phase difference network: two chains of four
// second order allpasses, y[n] = a^2 (x[n] + y[n-2]) - x[n-2]; the outputs are
// 90 degrees apart within 0.7 degrees from 0.002 to 0.998 of Nyquist
// (50 Hz .. 23.95 kHz at 48 kHz), at any sample rate.
allpass(a, x) = (\(y1).(a * a * (x + y1') - x'')) ~ _;
chain(a1, a2, a3, a4) = allpass(a1) : allpass(a2) : allpass(a3) : allpass(a4);
analytic(x) = x <: (chain(0.6923878, 0.9360654322959, 0.9882295226860, 0.9987488452737) : mem),
                   chain(0.4021921162426, 0.8561710882420, 0.9722909545651, 0.9952884791278);

// quadrature oscillator at f Hz (f may be negative)
phase(f)    = (+(f / ma.SR) : ma.frac) ~ _;
osc_sin(f)  = sin(2 * ma.PI * phase(f));
osc_cos(f)  = cos(2 * ma.PI * phase(f));

// the input shifted down and up by f Hz
shifter(f, x) = analytic(x) : \(re, im).(re * osc_cos(f) - im * osc_sin(f),
                                         re * osc_cos(f) + im * osc_sin(f));

// ---- one channel ------------------------------------------------------------

// input plus delayed feedback -> shifter -> sideband balance; outputs the
// main mix, up and down
MAX_DELAY   = 65536;   // 1.36 s at 48 kHz (PSRAM)
channel(f, x) = loop ~ fb_path : !, _, _, _
with {
    loop(fb_in) = x + fb_in : shifter(f) <: wet, (wet : main), up, down
    with {
        up(dn, u) = u;
        down(dn, u) = dn;
        wet(dn, u) = dn * (1 - sideband) + u * sideband;
        main(w) = x * (1 - mix) + w * mix;
    };
    // delay, lowpass (damp: 18 kHz .. 300 Hz) and a soft limit keep it stable
    fb_path     = de.fdelay(MAX_DELAY, delay_ms * ma.SR / 1000 - 1)
                : fi.lowpass(1, 300 * 60 ^ (1 - damp))
                : *(feedback * 0.98) : ma.tanh;
};

// ---- outputs ----------------------------------------------------------------

shift_bar   = shift_l : *(10) : rint : /(10) : vbargraph("shift Hz [status:1] [unit:Hz]", -999, 999);
show(x)     = x : attach(_, shift_bar);

// channels give main, up, down each; sorted to main L R, up L R, down L R
process(l, r) = (channel(shift_l, l), channel(shift_r, r)
                 : route(6, 6, 1, 1, 4, 2, 2, 3, 5, 4, 3, 5, 6, 6)
                 : show, _, _, _, _, _),
                // 0.95: with the output calibration (gain and offset) +-5 V ends beyond
                // the DAC range (CLIP)
                osc_sin(shift_l) * 0.95, osc_cos(shift_l) * 0.95;
