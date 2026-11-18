# strike

Six voice physical modelling percussion: mallet and noise exciters into modal resonators (string, beam, marimba, drumhead, membrane, plate); triggers on IN 1-6 and MIDI (a channel per voice, or notes 36-41).

Source: [`strike.dsp`](../strike.dsp), version 0.6.

Six drum voices, each an exciter striking a resonating object:

```
  - exciter: a mallet (an impulse, its stiffness a low-pass: soft and
    round .. hard and bright) and noise (attack/decay envelope, density
    from single grains to white noise, low- and high-pass), mixed
  - object: 6 modes (two-pole resonators) with the partial ratios of a
    string, beam, marimba bar, drumhead (circular membrane), membrane
    (rectangular) or plate. Decay, strike position (edge .. middle:
    which partials sound), tone (material: high partials decay faster
    for wood, as long as the low ones for metal), inharmonicity (partials
    closer together or spread), low cut, and a pitch envelope (bend)
  - noise out: the noise also straight to the output (snare wires, hats)
  - choke groups A/B: a voice silences the others of its group (hats)
```

Each voice's parameters are settings: "voice" selects which voice the
settings list shows and POT A-D edit (inharmonic, position, bend, tension). A knob
takes over when it is turned (jump), the settings are saved per voice.

```
in 1-6: triggers of voices 1-6 (rising edge above ~1 V)
MIDI (setting "midi"): channels: voice 1-6 on channels base..base+5 (the
        MIDI channel system setting, omni = 1), any note, C4 = the
        voice's pitch (the last note also sets the pitch for input
        triggers), with velocity; notes: notes 36-41 (C1-F1) on the
        base channel
out 1/2: stereo mix (soft clipped at 4.5 V)   out 3-8: voices 1-6 (mono)
```

```
hardware controls (rev2)                      rev1
X Y Z (knob + CV): assignable, for all voices (settings x, y, z: decay,
  hardness, drive, pitch, tone, noise, position, bend, tension, variation,
  rattle; default decay / hardness / drive)      rev1: CV X Y Z
POT A-D: inharmonic, position, bend, tension of the selected voice
  (rev1: CV A-D)
button 1: next voice (on release); held while turning POT A-D: all six
  voices move by the same amount; held while pressing button 2: a new
  random sound for the selected voice (played); button 2: plays the
  selected voice
LEDs: 1 the selected voice plays, 2 any voice; the home screen shows the
  selected voice, its type and a strip of the six voices
settings list: the selected voice's settings in sections object (type,
  pitch, decay, position, tone, inharmonic, low cut, bend, bend time),
  exciter (noise blend, stiffness, noise attack, noise decay, density, noise lp,
  noise hp), output (noise out, level, pan, choke), then kit (midi)
```
