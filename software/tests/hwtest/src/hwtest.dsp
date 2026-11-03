// hardware test program
// outputs: a DC level (dc0..dc7) plus a sine (sine0..sine7 amplitude, freq)
// per output, set over USB serial
// inputs: DC level (idc0..idc7, mean over ~20 ms) and AC RMS (irms0..irms7, ~0.3 s)
// of every input as bargraphs, printed over USB serial
import("stdfaust.lib");

freq = hslider("freq", 1000, 10, 20000, 1);
tau = ba.tau2pole(0.3);

// attach(x, y) outputs x and keeps y computed: the meters output 0.
// (attach(0) would be attach(_, 0): FAUST fills the last argument, which fed
// the meters back to the outputs)
silent = (0, _) : attach;
meter(i) = _ <: (si.smooth(ba.tau2pole(0.02)) : hbargraph("idc%i", -1, 1) : silent),
                ((_ <: _ - si.smooth(tau)) : ^(2) : si.smooth(tau) : sqrt : hbargraph("irms%i", 0, 1) : silent)
             :> _;

out(i) = hslider("dc%i", 0, -1, 1, 0.00001) + hslider("sine%i", 0, 0, 1, 0.0001) * os.osc(freq);

process = par(i, 8, meter(i) + out(i));
