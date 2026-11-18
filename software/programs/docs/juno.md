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
attack decay sustain release POT_A B C D             knob A B C D
everything else: settings, MIDI CC (numbers in the labels)
```
