# bitrot

Data errors and glitches: a recording buffer that stutters, jumps back, reverses, changes speed, stops like tape and drops out at random on the transport or a free clock; bit flips, crush, stalls and dropouts; repeatable random patterns.

Source: [`bitrot.dsp`](../bitrot.dsp), version 0.1.

Stereo in 1/2. Out 1/2 mix, 3 trigger (5 V, 5 ms) when an error starts,
4 random CV per event (0-5 V), 5/6 wet only. LED 1 lights during an
error, LED 2 while frozen.

The buffer records all the time (10.9 s per channel at 48 kHz). Time cuts
it into events: note values on the module's transport with sync (MIDI
clock, IN 8 clock or the internal tempo, nemesis.lib; a free clock at the
tempo while it is stopped), or a free length from 2 s to 20 ms. At every
event the dice decide, with the chance set by chaos, whether it is an
error and which one, weighted by the error settings:

```
  repeat     the first 1/repeats of the event plays and then loops
             (repeats 1..32: a beat repeat)
  jump       plays from further back in the buffer, a whole number of
             events back (depth: from 1 event to ~8 s), like a CD skip
  reverse    the past played backwards
  speed      ½x, reversed, 2x, reversed ½x, reversed 2x, ¼x (bend: how
             many of these are allowed, the gentle ones first)
  tape stop  the playback slows to a halt over the event
  gap        silence for 25..100 % of the event
```

Corrupt adds data errors to the wet signal all the time (also without
events), the type a setting:

```
  bits       16 bit samples with random bit flips (more and higher bits)
  crush      fewer bits and a lower sample rate
  stall      the signal hangs: a few ms repeat for 20-120 ms
  drop       short random dropouts
```

Pattern: the dice are a hash of the event number, the seed and the
channel, so with a pattern length the same errors come back every 2..32
events (synced: counted from the transport's bar, so the pattern sits on
the beat). A new seed, or a rising edge on reroll (a setting, assign a CV
to it), gives a new pattern. Free: never repeats.

Freeze: the buffer stops recording and loops the event length from the
moment it froze; the clean path plays that loop, the errors bend it.
Window: fades at the edges of the played pieces, 0 = hard cuts (clicks
are part of it), 1 = every piece swells in and out. Stereo split: each
channel rolls its own dice.

```
hardware controls (rev2)                      rev1
time, chaos, repeats    X Y Z (knob + CV)     CV X Y Z
corrupt, depth, bend    POT A B C (+ CV A-C)  CV A B C
mix                     POT D (+ CV D)        CV F
settings (turn the encoder): sync, freeze, pattern, seed, reroll, the
error weights, corrupt type, window, stereo
```
