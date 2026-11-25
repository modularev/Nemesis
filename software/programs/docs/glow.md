# glow

Vactrol band-pass and MOSFET drive in a feedback loop: low, high and band pass by width, warmth to fuzz, resonance to self-oscillation, LFOs and resonant CV processing.

Source: [`glow.dsp`](../glow.dsp), version 0.1.

A filter whose two edges are vactrols, a MOSFET style drive stage in front
of it, and a feedback path from the output back to the start. The edges set
the band: close together a resonant band-pass, far apart a low-pass (the
band placed low) or a high-pass (placed high). Feedback adds resonance,
above its threshold the loop oscillates: a sine-like tone, or in the LFO
range a slow wave. Everything is DC coupled, so it also shapes control
voltages: an envelope rings, a gate wobbles, a square LFO becomes a
resonant one.

```
  - frequency (X): the middle of the band, 20 Hz .. 18 kHz (range: lfo,
    0.02 .. 100 Hz); CV A adds 1 V/oct while it isn't the input
  - width (Y): how far the edges are from the middle, 0.08 .. 4 octaves
    each: narrow = a resonant band, wide = everything from the low edge to
    the high one (wide and low: a low-pass, wide and high: a high-pass).
    The middle always passes at 0 dB
  - feedback (Z): the output back into the drive: resonance, and from
    about 0.8 self-oscillation (the drive limits its level). More drive
    raises the loop's gain: it starts oscillating at lower feedback.
    Polarity (setting) inverts it: a different resonance and pitch
  - drive (A): 0 .. 40 dB into the MOSFET stage: gentle warmth (the second
    harmonic ~-30 dB), crunch, fuzz; the level stays within ~3 dB
  - vactrol (B): how fast the edges follow: snappy (2 ms up, 25 ms down)
    to lazy (60 ms up, 1.5 s down; slower still as they get darker). The
    two edges lag apart, so a moving band breathes
  - bias (C): symmetric clipping (odd harmonics) to lopsided (even
    harmonics, rougher, sputtering at high drive)
  - mix (D): dry and processed on out 1/2
  - input (setting): audio (IN 1/2) or cv a (the voltage on CV A, in
    volts: envelopes, LFOs, gates; both channels)
  - range (setting): audio or lfo for the frequency knob
  - spread (setting): the right channel's frequency offset, +-12
    semitones (stereo width, or two LFOs at different rates)
  - level (setting): -24 .. +12 dB on the processed signal
```

```
in 1/2: stereo audio            CV A: signal (input cv a) or 1 V/oct
out 1/2: mix (soft clipped at 4.5 V)
out 3/4: processed only, unclipped, DC coupled (1.0 = 5 V): CV

hardware controls (rev2)                  rev1
frequency, width, feedback   X Y Z (knob + CV)   CV X Y Z
drive                        POT A               CV A
vactrol, bias, mix           POT B C D (+ CV B C D)   CV B C D
settings: input, range, spread, feedback polarity, level
```

Recipes: a resonant low-pass: width up, frequency low, feedback 0.6. An
LFO: range lfo, feedback 0.9, nothing patched; spread for a second rate on
out 4. A ringing envelope: input cv a, range lfo, frequency where it should
wobble (a few Hz), width narrow, feedback 0.6-0.75.

## How it works

Per channel: input + feedback -> drive stage -> a high-pass and a low-pass
state variable section (TPT) -> output, which is fed back one sample later.
The drive stage is an asymmetric soft clipper (a rational tanh, the
negative side earlier with more bias), its bias offset taken off again so
silence stays at 0 V. Each edge follows its target with a vactrol's
asymmetric lag; the filter coefficient comes from a table over 21 octaves
(no tan or pow per sample). A narrow band's loss is made up exactly, so
the oscillation threshold doesn't move with the width. An oscillation
grows from the noise floor by a few percent per period (minutes at 0.3 Hz):
while the feedback is above the threshold and the output has been quiet
for two periods, a small step goes into the loop and it starts at once.
