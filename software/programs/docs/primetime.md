# primetime

Two tap digital delay after the Lexicon Prime Time II (Model 95): one delay line with taps A and B, recirculation with filter and DRC, VCO time base sweep (manual, LFO, envelope), X2 range, infinite repeat, phase invert, input/output mix, clock out.

Source: [`primetime.dsp`](../primetime.dsp), version 0.1.

After the Model 95 owner's manual (block diagram Fig. 3.22, modulation
Fig. 3.28, specifications section 6):

```
  IN 1 = MAIN, IN 2 = AUX -> input limiter -> + recirculation -> anti-alias
  low-pass (16 kHz, X2 8 kHz) -> converter (clips at full scale) -> one
  delay line with taps A and B -> reconstruction low-pass. Recirculation:
  A x A-FB (phase invert) + B x B-FB -> 6 dB/oct low-pass 800 Hz-20 kHz ->
  DRC. Output mix: MAIN (phase invert), AUX, A, B (phase invert), MASTER.
```

Delay select: 128 settings per tap, roughly exponential (0.1 ms steps
at the start, ~4-5 % per step above 10 ms), rounded to prime numbers of
0.1 ms like the unit's "prime" divisions (fewer comb resonances between
the taps), up to the memory size (standard 640 ms, option 1 1.28 s,
option 2 2.56 s; X2 doubles them). With sync the taps are note values on
the module's transport instead.

VCO (the time base, not the taps: both delays sweep together, so the
pitch bends like a tape speed change): manual sweep 0.5-1.5x of the set
delays; depth up to 3:1 (0.5-1.5x) and the manual range shrinks as depth
grows (at full depth manual sweep does nothing), as on the unit; rate
0.05-20 Hz, LFO sine or square; shape blends the LFO with an envelope
follower on the input mix (loud = shorter delay). xtal: fixed time base,
sweep and modulation off.

DRC (dynamic recirculation control): the recirculation goes down while
the input plays and comes back up to the A-FB/B-FB levels when it falls
below a threshold: dry phrases, long tails in the gaps.

Infinite repeat (setting, assign a CV gate to it): the memory stops
recording and loops the longest delay the delay select allows (memory
size x range x sweep at that moment). The taps then only offset where
A and B read in the loop (two staggered copies); the sweep changes the
speed and pitch of the loop like tape. Off: the loop decays by the
recirculation.

Clock (the unit's metronome): the loop time is the longer tap delay (or
the repeat loop); out 8 pulses once per beat (beats per loop), out 7
pulses per beat times that, 5 V. LED 1 flashes on the beat, LED 2 while
repeating. The home screen shows the delay times of A and B in ms.

Outputs: 1/2 master (outputs 'mono': the master on both; 'A/B': left
with tap A, right with tap B), 3 delay A, 4 delay B, 5 input mix (what
feeds the delay line), 6 mod (the shape output, 0-5 V), 7/8 clock.

```
hardware controls (rev2)                         rev1
delay A, delay B, manual sweep  X Y Z (knob+CV)  CV X Y Z
A-FB, B-FB, depth, rate         POT A-D (+CV A-D) CV A-D
shape, master                   settings          CV E, CV F
settings: sync, range, memory, vco, lfo, shape, repeat, input mix, recirculation filter,
drc, output mix, outputs, phase invert, bypass, limiter, beats, pulses
```
