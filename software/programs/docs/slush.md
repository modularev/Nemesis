# slush

Spectral freeze and smear (a phase vocoder): holds the input as an endless sound that keeps moving, or smears it continuously; glides between freezes, drift, purity, layers, pitch, grain.

Source: [`slush.dsp`](../slush.dsp), version 0.5. The spectral part is in the library: [`nemesis_spectral.h`](../../lib/nemesis/src/nemesis_spectral.h).

The first program with FFT processing. A freeze takes a snapshot of the
input's spectrum (2048 points, 43 ms by default) and plays it on: every frequency
keeps its level and keeps turning at its own speed, so a held chord or a
noise stays as it was, without the loop seam of a sampler.

```
  - freeze: button 1 toggles it; a gate on CV A (rev1: knob/CV E) freezes
    while high, and every rising edge captures anew. LED 1 shows it
  - a freeze holds the input's levels averaged over its last ~150 ms
    (geometric: the sound's steady timbre, not one window's moment, and
    attacks don't lift it), each partial turning on at its own frequency:
    the sound sustains endlessly at the level it had
  - speed: a new freeze glides from the sound before: every partial's
    level and pitch move to the nearest new partial (within half an
    octave) over 0 .. 60 s (0 at the left end: at once); partials without
    a partner fade. A release fades out as long. The new partials start
    at the old ones' phases, so the glide is an interpolation, not two
    copies beating against each other. A freeze during a glide takes over
    from where it is
  - drift: every partial's pitch wanders slowly (up to +-50 cents, a new
    random target every ~0.3 s) and its level (+-4 dB, every ~0.7 s),
    both rising with drift^2 (0.5: +-12 cents):
    from a still copy to a living, chorused one
  - scatter (default 0): the freeze also keeps the last 32 frames of the
    input (85 ms at 512 points, 170 ms at 1024, 340 ms at 2048, 680 ms at
    4096), every bin's level and frequency. With scatter every bin plays,
    frame by frame, a frame chosen at random from the newest 2 .. 32: its
    level (smoothed over scatter time, 0.05 .. 3 s, default 0.3) and, as
    far as scatter goes, its frequency as measured there: each bin drifts on its own, no common movement and
    no repeating wobble. A freeze right after the previous one holds only
    what came since, and scatter stays within that
  - lock (setting, default 1): 1 = every frozen bin turns with its
    partial's peak (clean, precise partials); lower, each bin moves
    towards its own frequency (with scatter: the random frame's) and its
    phase wanders slowly on its own: softer, diffuse, lush. Lock 0 with
    scatter up is the closest to a classic random-frame freeze
  - smear: every frequency follows the input with inertia, 0 (none: the
    input resynthesized) .. 20 s: a continuously smeared, morphing copy
    of the input, always on (mix sets how much is heard); freeze
    crossfades from it into the held sound and back. tilt
    (setting): highs follow faster (left) or slower (right) than lows.
    Every bin is an oscillator that keeps its frequency and phase; it
    learns them only while the input carries it steadily as a tone, so a
    fading note keeps its own pitch and partials don't step or flicker
  - purity: only the strongest partials sound: 0 = all (the original
    timbre), turning it up drops the weaker ones (-90 dB .. -1 dB under
    the loudest, a soft edge); from 0.7 every partial also narrows to
    fewer bins (its peak +-1 at 1), down to a single, electronic tone.
    Frozen sound and smear
  - layer (setting): the previous capture stays under the new one at
    this level (two layers, kept at equal loudness)
  - grain (setting): random variations of each component's level and
    phase within its bin, every frame: from clean tones to a grainy,
    noisy version of the frozen sound; stronger at low resolutions
    (wider bins)
  - pitch (setting, -24 .. +24 semitones): moves every partial (peaks
    and their bins move together, so the timbre stays); it glides over
    ~30 ms. With smear off and not frozen it is a live pitch shifter
  - resolution (setting): 512, 1024, 2048 (default) or 4096 points (11,
    21, 43, 85 ms at 48 kHz). More points: finer in frequency (closer
    notes stay tonal, low chords resolve), smeared more in time (attacks
    soften). 4096 is one analysis: two channels of it don't fit the fast
    memory, so both inputs go into one; its bins are spread over the
    stereo field (each at a fixed random place), so width still works. Changing it restarts the
    spectral part (a short silence)
  - partials that stay stable are held with their phase; where notes are
    closer than the resolution (a low chord) or the sound is noisy, the
    bins get a new random phase every frame: they keep their level as
    texture instead of playing tones that weren't there
  - out 1/2: the input; frozen, the input crossfades into the frozen
    sound (over speed, equal power). mix sets how much of the input stays under it:
    fully wet = only the frozen sound while frozen, fully dry = only the
    input. The input itself is never in the FFT (no delay, no comb).
    out 3/4: the frozen sound alone
```

```
in 1/2: stereo audio
out 1/2: mix (soft clipped at 4.5 V)   out 3/4: frozen sound only

hardware controls (rev2)                 rev1
drift, scatter, speed  X Y Z (knob + CV)  CV X Y Z
mix                    POT A              CV A
level, purity, smear   POT B C D (+ CV B C D)   CV B C D
settings: scatter time, tilt, resolution, width, tone, layer, grain, pitch, lock
freeze                 button 1, gate on CV A   button 1, knob/CV E
speed 0 .. 60 s; level 0 .. 2x; tone a low-pass (left) or high-pass (right)
on the frozen sound and the smear
```

## How it works

An STFT: 512 to 4096 point frames every N/4 samples (4x overlap, Hann windows),
CMSIS-DSP's real FFT. The output is N + N/8 samples behind the input;
it only carries the frozen sound, so the delay doesn't matter. At a capture
each frequency bin keeps its magnitude, its phase and its phase turn per
frame (this frame times the conjugate of the previous one); each frame then
turns every bin by that amount. No atan2, sin or cos per bin: about 3.7 %
CPU while not frozen (the forward FFTs keep the last frame ready), more
while frozen.

Each frame's synthesis runs in four steps an eighth of a hop apart (the
smear, layer 0, layer 1, the inverse FFT with the overlap-add), so a frame's
work spreads over several audio blocks. The buffers are in the part of the
library's RAM1 arena the program leaves free and in RAM2; only the level
history of 32 frames is in PSRAM (a frame touching PSRAM took more than an
audio block at 4096 points). rev2, 48 kHz, block 128, average (peak of a
block): 512, 1024 and 2048 points ~13 % idle, 23-25 % (33-37 %) with
smear; 4096 mono 9 % (39 %) idle, 14 % (59 %) with smear.

Where two partials share a bin its turn is a mixture: a frozen chord loses
about 1.5 dB over 10 s (a single tone 0.2 dB); drift and evolve move it
anyway.
