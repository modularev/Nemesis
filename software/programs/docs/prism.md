# prism

Spectral delay: every frequency band through its own delay, skewed from the lows to the highs, with feedback, scatter and freeze.

Source: [`prism.dsp`](../prism.dsp), version 0.1. The spectral part is in the library: [`nemesis_spectral.h`](../../lib/nemesis/src/nemesis_spectral.h) (kernel DELAY).

The input is split into its frequency bands (an FFT of 512 to 8192 points);
every band goes through its own delay line and comes back with feedback.
With all bands at the same time it is an echo that smears transients a
little; skewed, the highs come back before the lows or after them, so a
click becomes a falling or rising sweep, a chord unfolds note by note and
feedback builds up spectral trails.

```
  - time (X): 0.02 .. 3 s, the longest delay a band gets
  - skew (Y, -100 .. 100): 0 = every band the same time; -100 = the time
    rises towards the highs (the lowest band comes at once, the highest
    after the full time); +100 = towards the lows. Between: part of both
  - feedback (Z): how much of a band's echo feeds its next one (up to
    0.98; each band is held below full scale, so it can't run away)
  - scatter (POT B): every band's time moves by its own fixed random
    amount (up to half the time): a smear of echoes instead of a sweep
  - damp (POT C, -1 .. 1): the feedback falls towards the highs (right:
    they die away sooner, darker trails) or the lows (left: brighter)
  - freeze: button 1 toggles it, a gate on CV A (rev1: knob/CV E) holds
    it; no more input, every band loops its delay as it is (LED 1)
  - curve (setting): skew over frequency by octaves (default: every
    octave gets the same share of the sweep) or linearly (most of the
    bands are high: the lows come almost at once)
  - resolution (setting): 512 .. 8192 points (more: finer in frequency,
    5.9 Hz per band at 8192, softer in time; the output is N + N/8
    samples late: 192 ms at 8192)
  - tone (setting): a low-pass (left) or high-pass (right) on the delays;
    width (setting): the stereo spread of the delayed sound (the left and
    right scatter differently)
  - out 1/2: the input and the delays (mix, equal power; the input never
    goes through the FFT), out 3/4: the delays alone
```

```
in 1/2: stereo audio
out 1/2: mix (soft clipped at 4.5 V)   out 3/4: delays only

hardware controls (rev2)                 rev1
time, skew, feedback   X Y Z (knob + CV)  CV X Y Z
mix                    POT A              CV A
scatter, damp, level   POT B C D (+ CV B C D)   CV B C D
settings: curve, resolution, width, tone
freeze                 button 1, gate on CV A   button 1, knob/CV E
```

## How it works

An STFT (Hann windows, a frame every N/4 samples); every frame's spectrum
goes into a ring of the last 3 s (PSRAM). Each band of the output frame
is read from its own number of frames back (rounded to whole frames:
N/4 samples, 2.7 ms at 512 points, 43 ms at 8192), the input plus that
echo times the feedback is written back, and the delayed spectrum is
resynthesized. Each band keeps the phases it had, so a steady tone comes
back clean; bands of one sound with different times smear it in time.
