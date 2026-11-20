# slush

Spectral freeze and smear (a phase vocoder): holds the input as an endless sound that keeps moving, or smears it continuously; drift, crossfades.

Source: [`slush.dsp`](../slush.dsp), version 0.1. The spectral part is in the library: [`nemesis_spectral.h`](../../lib/nemesis/src/nemesis_spectral.h).

The first program with FFT processing. A freeze takes a snapshot of the
input's spectrum (1024 points, 21 ms) and plays it on: every frequency
keeps its level and keeps turning at its own speed, so a held chord or a
noise stays as it was, without the loop seam of a sampler.

```
  - freeze: button 1 toggles it; a gate on CV A (rev1: knob/CV E) freezes
    while high, and every rising edge captures anew. LED 1 shows it
  - a new capture crossfades from the previous one, a release fades out
    (fade)
  - drift: every partial's pitch wanders slowly (up to +-15 cents, a new
    random target every ~0.3 s): from a still copy to a chorused one
  - motion: the freeze holds 170 ms of the input and plays it back and
    forth (motion: how far back, speed: 0.05 .. 2 times a second)
  - smear: every frequency follows the input with inertia (0.05 .. 20 s;
    off at the left end): a continuously smeared, morphing copy of the
    input; freeze crossfades from it into the held sound and back. tilt
    (setting): highs follow faster (left) or slower (right) than lows
  - resolution (setting): 1024 or 2048 points (43 ms at 48 kHz, finer in
    frequency: closer notes stay tonal). Changing it restarts the spectral
    part (a short silence)
  - partials that stay stable are held with their phase; where notes are
    closer than the resolution (a low chord) or the sound is noisy, the
    bins get a new random phase every frame: they keep their level as
    texture instead of playing tones that weren't there
  - out 1/2: the input; frozen, the input crossfades into the frozen
    sound (over fade). mix sets how much of the input stays under it:
    fully wet = only the frozen sound while frozen, fully dry = only the
    input. The input itself is never in the FFT (no delay, no comb).
    out 3/4: the frozen sound alone
```

```
in 1/2: stereo audio
out 1/2: mix (soft clipped at 4.5 V)   out 3/4: frozen sound only

hardware controls (rev2)                 rev1
drift, motion, fade    X Y Z (knob + CV)  CV X Y Z
mix                    POT A              CV A
level, tone, smear     POT B C D (+ CV B C D)   CV B C D
settings: speed, tilt, resolution, width
freeze                 button 1, gate on CV A   button 1, knob/CV E
fade 0.02 .. 8 s; level 0 .. 2x; tone a low-pass (left) or high-pass (right)
```

## How it works

An STFT: 1024 point frames every 256 samples (4x overlap, Hann windows),
CMSIS-DSP's real FFT. The output is 1024 samples (21 ms) behind the input;
it only carries the frozen sound, so the delay doesn't matter. At a capture
each frequency bin keeps its magnitude, its phase and its phase turn per
frame (this frame times the conjugate of the previous one); each frame then
turns every bin by that amount. No atan2, sin or cos per bin: about 3.7 %
CPU while not frozen (the forward FFTs keep the last frame ready), more
while frozen.

Where two partials share a bin its turn is a mixture: a frozen chord loses
about 1.5 dB over 10 s (a single tone 0.2 dB); drift and evolve move it
anyway.
