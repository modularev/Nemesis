# harmonizer

Pitch changer and delay after the Eventide H949 Harmonizer: 1 octave up to 2 octaves down, two splice algorithms, micro pitch change, extended and reverse segments, random delay (ADT), flange, repeat, feedback with EQ; manual, MIDI keyboard or V/Oct.

Source: [`harmonizer.dsp`](../harmonizer.dsp), version 0.1.

After the H949 instruction manual (control description, mode chart on
page 9, feedback EQ response on page 12, specifications). The manual's
separate technical section (theory of operation) isn't part of it: the
two pitch change algorithms follow what the manual says about them.

One mono memory (IN 1, or IN 1 + 2): out 1 MAIN (the function: pitch
change, delay, random, flange, reverse), out 2 DELAY ONLY. Both read the
same memory, which also takes the feedback; 15 kHz bandwidth, the
converter clips at full scale.

```
Functions (setting "function"):
  delay      main = delay 0-393.75 ms in 6.25 ms steps (main delay)
  random     the main delay wanders between 0 and 25 ms at a constant
             rate towards random targets (manual = rate): automatic
             double tracking, slightly out of tune
  flange     the main output is the input plus a copy swept 0-10 ms
             (manual = sweep rate)
  reverse    pieces of the memory (400/200/100/50 ms, main delay)
             played backwards, the pitch set by the ratio
  normal     pitch change .25-2.00 (manual), splice window 25 ms; main
             delay adds 0-350 ms in 50 ms steps
  extended   pitch change over a longer window, the whole memory (400 ms)
             or 200/100/50 ms (main delay): smoother, more smeared
  sharp/flat micro pitch change 1.000-1.07 / 1.000-.93, fine near unity
             (manual fully left = 1:1)
```

Algorithm (setting): 2 = one read pointer that jumps back by the window
when it reaches its end, an audible splice ("glitch") whose rate grows
with the distance from 1:1 (as the H910); 1 = two pointers half a window
apart, crossfaded, no glitches but some colouring (comb). In random and
flange, algorithm 1 adds a second, later copy (the manual's "extra
delayed signal").

Control (setting): manual = the manual knob; keyboard = MIDI notes, C4
= 1:1, a key sets a musical interval (with glide, as the HK940); cv+man
= manual plus V/Oct (assign a CV input to "v-oct" in the program
settings; rev1: CV E). Quantize rounds the manual ratio to semitones.

Feedback: main level and delay only level (1 = unity loop gain with the
EQ centred, beyond: it runs away, as on the unit), low EQ (left: 300 Hz
high-pass, right: rising bass), high EQ (left: 2.5 kHz low-pass, right:
+14 dB above 2 kHz). Repeat (setting, assign a CV gate to it): the memory
stops taking the input and recirculates its 400 ms; the functions keep
working on it (a repeated phrase, pitch shifted).

The home screen shows the pitch ratio. LED 1 lights while repeating,
LED 2 flashes at every splice of algorithm 2.

```
hardware controls (rev2)                             rev1
manual, main delay, delay only    X Y Z (knob + CV)  CV X Y Z
main FB, delay only FB, low, high POT A-D (+ CV A-D) CV A-D
settings: function, algorithm, control, quantize, glide, repeat, input,
dry, main level, delay only level, line (bypass)
```
