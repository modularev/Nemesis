# polysynth

Oscillator experiments. `src/superosc.dsp` is active, the other programs are
parked as `.bdsp` so they are not compiled; rename one to `.dsp` and include
its header in `src/main.cpp` to try it.

superosc binds by label prefix: pitch, cutoff and resonance on CV_X, CV_Y and
CV_Z, oscillator levels and detune on POT_A..POT_D (on rev1 these are the
A..D knobs). Build with `pio run -e rev2 -t upload` (or `-e rev1`). The project
runs the Teensy at 816 MHz and prints the CPU load every second.
