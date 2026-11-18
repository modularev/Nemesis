# juno

Juno style polysynth: DCO saw/pulse/sub/noise, HPF, 4 pole VCF, ADSR, LFO, chorus I/II.

Source: [`juno.dsp`](../juno.dsp), version 0.17.

Play it over MIDI (USB or the MIDI jack), stereo out on outputs 1/2.
Every voice sits at its own place in the stereo field (spread), and notes
move through the voices, so chords and repeated notes open up; the chorus
then works on both sides.

```
hardware controls            rev2                    rev1
cutoff resonance env amount  X Y Z (knob + CV)       CV X Y Z
attack decay sustain release POT_A B C D (+ CV A-D) knob A B C D
LFO rate, LFO VCF depth      settings                knob E F
everything else: settings, MIDI CC (numbers in the labels)
```

A patch sets every parameter. On rev2 a knob takes over a patch's value
when it reaches it; rev1's knobs are inputs (knob + CV summed in hardware),
so there the knobs' positions win over the patch for X Y Z, A-D and E F
(the remote page marks the patch's values on those knobs).
