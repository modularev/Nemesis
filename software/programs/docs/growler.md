# growler

Stereo 4-operator FM bass built around cross modulation, operators played by keys or V/Oct: a depth knob per operator, carrier and operator 4 modulating each other, feedback, 16 ratio sets, ladder filter; MIDI or V/Oct + gate.

Source: [`growler.dsp`](../growler.dsp), version 0.7.

One voice in stereo:

```
  4 sine operators in one of 6 algorithms (mode), carrier and operator 4
  modulating each other (cross) --> wavefolder --> 24 dB ladder filter
     --+--> amp envelope --> out 1 (left), the same detuned --> out 2
  sub sine (one octave below the carrier, clean, centred) --+
```

width detunes the left and right networks against each other (0: mono).

```
Playing the operators (setting keys), each at its pitch times its ratio:
  spread       the operators are shared by the notes played together: one
               note all four, two notes 1+2 and 3+4, three notes 1+2 for
               the lowest and 3, 4 for the others, four notes one each
               (in order of pitch); lifting keys keeps it, a note played
               alone starts anew. In twin mode two notes are two complete
               FM voices (2 -> 1, 4 -> 3), in parallel mode a chord.
  round robin  a note played alone sets all four operators; notes added
               while keys are held go to the next operator in turn (2, 3,
               4, 1 ..) and stay when lifted, until the next note played
               alone sets all four again
  held         the first held key is the carrier, operators 2..4 follow it;
               a second, third, fourth key held at the same time sets
               operator 2, 3, 4 to its pitch while it is held
The intervals between the notes become the FM ratios (in parallel mode a
four note chord). Each operator also has a V/Oct target, v-oct 1..4 (CV
A-D as assigned in the program settings): v-oct 1 transposes everything,
v-oct 2..4 offset their operator on top.
```

cv gate (or any key) opens the envelope. vca off: no envelope, the sound
keeps going (drone), keys and V/Oct only set the pitches.

The knobs play the modulation network: mod 2/3/4 set how much each
operator takes part (how hard it modulates, or, where the mode makes it a
carrier, its level and how hard it is modulated), cross lets the carrier
and operator 4 modulate each other (both ways, in every mode: from grit
to chaos), feedback feeds operator 4 and the carrier into themselves
(towards saws, then noise), ratios steps through 16 sets of ratios for
operators 2, 3, 4: round and saw-like, harmonic and bright, hollow (odd
harmonics), then inharmonic and metallic.

```
Algorithms (setting mode), modulators -> carriers:
  stack   4 -> 3 -> 2 -> 1        the most aggressive, metallic
  Y       3 -> 2 -> 1, 4 -> 1     two paths into the carrier
  twin    2 -> 1, 4 -> 3          two voices, carrier 3 at its ratio
  fan     4 -> 1, 2, 3            one modulator, three carriers
  branch  4 -> 3 -> 1, 2 -> 1     a bright path and a direct one
  three   2, 3, 4 -> 1            three modulators on one carrier, thick
  parallel 1, 2, 3, 4 all carriers  four tones side by side (a chord from
                                    four keys or v-oct 1..4); mod 2/3/4
                                    set their levels, cross and feedback
                                    still bend carrier and operator 4
```

The depths follow the knobs only (no envelopes) and drop above ~110 Hz so
high notes stay clean. A decay envelope, restarted on every new note
(legato and chords too), opens the filter (env filter, filter decay).

```
in: none; V/Oct 1..4 and gate on CV A-D as assigned (rev2, V/Oct
    calibrated; gate > 4 V or 2.5 V with the 0-5 V range), rev1: V/Oct 1
    on CV E, gate on CV F
out 1/2: audio left/right
out 3: filter envelope, 0..5 V         out 4: amp envelope, 0..5 V
```

```
hardware controls (rev2)                          rev1
mod 2, mod 3, mod 4         X Y Z (knob + CV)     CV X Y Z
V/Oct 1, gate               CV A, CV B (assignable) CV E, CV F
cross, feedback             POT A, POT B          CV A, B
ratios, cutoff              POT C, POT D (+ CV C, D)   CV C, D
settings (turn the encoder): mode, keys, vca, width, drive, resonance,
  env filter, filter decay, sub, octave, glide, attack, release,
  velocity, level
MIDI: notes (spread, round robin or up to 4 held keys), velocity (last key), pitch
  bend (+-2 semitones)
```
