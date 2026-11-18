# superosc

Drone generator: root, fourth and fifth, each as 4 detuned saw partials, through a ladder filter; MIDI note and V/Oct.

Source: [`superosc.dsp`](../superosc.dsp), version 0.2.

A stereo drone: three oscillator groups (root, fourth, fifth), each made of
four saw partials at 1, 2, 3 and 4 times its frequency. Detune spreads the
partials by a few Hz (mirrored left and right), so they beat at the same
rate at any pitch; drift adds a slow, random-like wander. A ladder filter
per channel and a limiter keep the output below 4.5 V.

Pitch = MIDI note (default C2) + octave + tune + pitch bend + V/Oct.
The last MIDI note stays when it is released: the drone keeps sounding.
With "gated" on, MIDI notes swell the drone in and out instead.

```
in: none; V/Oct on CV A (rev2, calibrated) or CV E (rev1), 1 V per octave,
    0 V = the MIDI note
out 1/2: stereo audio (limited to 4.5 V)
out 3: pitch CV, 1 V/octave, 0 V = C4 (MIDI to CV: note, tune, bend, V/Oct)
out 4: MIDI gate, 5 V while a note is held
```

```
hardware controls (rev2)                         rev1
tune, cutoff, resonance   X Y Z (knob + CV)       CV X Y Z
V/Oct                     CV A (calibrated)       CV E (0..1 taken as 0..5 V)
root, fourth, fifth       POT A, B, C (+ CV B, C)  CV A B C
detune                    POT D (+ CV D)          CV D
drift                     setting                 CV F
settings (turn the encoder): octave, drift (MIDI mod wheel), gated, swell, level
MIDI: note, pitch bend (+-2 semitones), CC 1 drift
```
