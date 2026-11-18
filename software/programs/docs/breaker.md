# breaker

Beat slice repeat and reverse for breakbeats, bit crush, comb filter and drive, plus six drum trigger outputs from a 2D pattern map; MIDI clock or internal tempo.

Source: [`breaker.dsp`](../breaker.dsp), version 0.3.

Stereo in 1/2, out 1/2. Outs 3-8: drum triggers (5 V) kick, snare, closed
hi-hat, open hi-hat, perc 1, perc 2. LED 1 flashes on the beat, LED 2
lights while a slice repeats.

The slice grid and the drum steps follow the module's transport
(nemesis.lib): the MIDI clock, a clock or gate on audio IN 8 (clock
"IN 8", 1/4 to 1/16 or 24 pulses per quarter) or the internal tempo, set
in the system settings (press and hold the encoder). While the transport
is stopped the
input passes through and the triggers are silent.

```
Slicer: at every slice boundary the program decides whether that slice
  repeats: the slice is recorded once and then looped until repeat ends;
           shortening the slice while repeating rolls the start of it
  reverses: plays the previous slice backwards
The rev-rep knob rolls the dice per slice: right of the centre for repeats,
left for reverses, the centre does neither; fully turned = every slice.
The gates force it.
```

Comb: centre off; turning either way raises the feedback and the pitch
(40 Hz .. 1.9 kHz). Left is negative feedback (hollow, an octave lower,
odd harmonics), right positive (metallic, resonant).

Drums: a 4 x 4 map of one bar patterns (16 sixteenths). Pattern x moves
through styles (funk, boom bap, break, broken), pattern y from sparse to
busy. Between the corners the patterns blend: every step has a weight
0..9 and fires when the blended weight passes the density threshold, so
moving a knob adds and drops ghost notes gradually. An open hi-hat chokes
the closed one on the same step.

```
hardware controls (rev2)
slice length, rev-rep, comb                   X Y Z (knob + CV)
repeat gate, reverse gate                     CV A, CV B (> 4 V, or 2.5 V with the 0-5 V range)
crush, drive                                  POT A, POT B
pattern x (+ CV C), pattern y (+ CV D)        POT C, POT D
settings (turn the encoder): mix, density, swing, trig length
```
