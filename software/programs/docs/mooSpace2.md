§# mooSpace2

Moving spaces: mooSpace with wider controls and glide (pitch) or crossfade (no pitch) when the space moves.

Source: [`mooSpace2.dsp`](../mooSpace2.dsp), version 2.8.

mooSpace v1's algorithm (input diffusion, a figure eight tank with
diffusers in the feedback, multi tap allpasses), with:

```
  - wider control ranges; the knob centres sit where v1's defaults were
    smear: how much the sound is blurred, the strength of all diffusers
           (input and tank): left hardly any (clear, separate
           reflections), centre v1, right dense (coefficients up to 0.9);
           it also keeps the diffusers long enough to spread transients
           (at least 2.5x the base lengths fully right), even at low drag
    drag:  how far the space stretches in time, the lengths of the
           diffusers (0.05x .. 9.5x, centre 4.75 as v1) and the late taps
           (up to 2x v1): left tight and short, right long and dragging
    modulate (setting, assignable to CV A-D): deeper, partly scaled with
             the lengths, clockwise faster
  - shimmer: a pitch shifter (setting shift, +12 by default) where the
    input and the feedback enter each half of the tank: with a short
    decay the input comes in shifted (an octave reverb), with a long one
    every pass goes up again (the rising shimmer)
  - tone, in the tank (its input and its feedback loops): the centre is
    v1's default colour; to the left the damping in the loops grows and
    the input darkens (down to 150 Hz), to the right high-passes in the
    input and the loops thin the space (up to 2.5 kHz)
  - the tank's level is held by soft limiters without time constants
    (v1: slow compressors, which pumped at ~2 Hz with long decays and loud
    signals), and each half has one more allpass (fixed, prime lengths):
    more, closer resonances, so held tones beat less in the tail
  - reverse: the signal going into the tank comes in reversed pieces of
    a third of a second (two heads, crossfaded); the knob fades from
    forward (0) to reverse echoes (1)
  - sync (setting): the times lock to the transport's tempo (internal,
    MIDI clock or IN 8): reverse pieces are a note value (reverse length),
    lag a note value (the menu shows note values instead of ms), and the main late taps (drag) the
    nearest multiple of a 1/32, so those echoes fall into the rhythm
  - motion (setting): the delays in the tank have two read heads on one
    buffer. on glides like v1 (pitch shifts while smear or modulate move
    the space), off crossfades over 25 ms (no pitch, a grainy stepping).
    The input diffusers too; lag always glides (CPU).
```

```
  - freeze, echoes, duck, gate, lofi (settings), see their definitions
```

```
in 1/2: stereo audio    out 1/2: stereo mix (limited to 4.5 V)
  out 3/4: rear (wet, decorrelated)   out 5/6: wet only
  out 7: tail envelope, out 8: input envelope (CV 0..5 V)
```

```
hardware controls (rev2)                      rev1 (knob + CV)
smear decay drag    X Y Z (knob + CV)          CV X Y Z
reverse shimmer     POT A, POT B (+ CV A, B)   CV A, B
tone mix            POT C, POT D (+ CV C, D)   CV C, D
settings (turn the encoder): sync, reverse length, freeze, echoes, echo
  time, duck, gate, lofi, modulate, shift, lag (ms, or a note value with
  sync), push, motion
  (rev1: push CV E, lag CV F)
```
