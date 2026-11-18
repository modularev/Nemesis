# pollen

Live granular sampler: grains from a looping live memory (freeze holds it), position, size, pitch, density, spray, blur reverb; free or synced to the transport.

Source: [`pollen.dsp`](../pollen.dsp), version 0.3.

A prototype granulator for live input only.

```
  - live memory: a loop per channel (2/4/8/10 s). While recording it
    always holds the last seconds of IN 1/2; freeze fades the input out
    (50 ms) and the loop keeps repeating itself, so grains can wander
    through it for as long as it stays frozen. A frozen loop is saved on
    the SD card (/NEMESIS/pollen.WAV, 32 bit float) and is there again
    when pollen starts with freeze on (after another program or power off)
  - grains: 8 voices. Each grain takes its start, size, pitch, direction
    and pan when it starts and keeps them (a snapshot of the controls);
    window shapes bell/tukey/decay/ramp/square/triangle
  - rate: density = overlapping grains (0.25 .. 7, the rate follows the
    size); with sync and a running transport POT A picks a note value
    (1 bar .. 1/128) and grains start on that grid
  - position: CW reads the newest audio, CCW further back; scan moves the
    start through the memory (while frozen: 0 holds still, 1 plays at
    the original speed); spray scatters the start randomly
  - blur: a small reverb after the grains, send and decay on one knob
```

```
in 1/2: stereo audio
out 1/2: mix (limited to 4.5 V)   out 7/8: wet only
out 3: trigger per grain (5 ms, 5 V)   out 4: envelope of the grains
out 5: random value per grain (0..5 V)  out 6: pitch of the latest grain
       (1 V/oct around 0 V)
```

```
hardware controls (rev2)                      rev1
position size pitch   X Y Z (knob + CV)        CV X Y Z
density spray         POT A, POT B (+ CV A, B) CV A, B
blur mix              POT C, POT D (+ CV C, D) CV C, D
reverse tone          settings                 CV E, F
settings: freeze, memory, sync, scan, reverse, pitch spray, quantize,
  shape, pan spread, tone, blur input (the input into the blur, heard
  also with mix fully dry)
```

## Looper

- **loop out** plays the frozen memory directly: the loop as it is, no
  grains, no windows. It is part of the wet signal (wet = grains + loop out
  x loop), so mix (POT D) fades between the input and grains plus loop;
  outs 7/8 carry it too. While recording it is silent.
- **memory** with sync on is a number of bars (1, 2, 4, 8) at the
  transport's tempo, halved while they don't fit in the memory (8 bars at
  120 BPM are 16 s, the memory holds 10.9 s); with sync off seconds as
  before.
- **freeze** with sync on and a running transport takes effect on the next
  bar line (on and off): the loop holds exactly the bars before it.
- **overdub** adds the input into the frozen loop; **loop feedback** is how
  much of the loop each pass keeps (1: forever, lower: it fades). The loop
  is saved on the SD card when it was frozen; later overdubs are not saved.
  With feedback 1 every overdub adds to the loop; it saturates softly at
  full scale (no hard clipping), but feedback around 0.8-0.9 keeps it clean.

Grains keep playing over the same memory, so the looper and the granulator
can run together.

## Grain size

The size knob goes from 20 ms to the memory length (2/4/8/10 s, setting
memory), exponentially. A grain must fit in the memory with its pitch:
it reads size x pitch ratio of audio while the memory moves on, so it is
at most memory / (1 + ratio) long: half the memory at unison (5 s at
10 s), 80 % at -24 st (8 s), a fifth at +24 st (2 s). With sync, a grain
lasts at most 7 grain intervals (at 1/128 and 120 BPM about 110 ms).

## Memory and grain budget

The live memory holds 16 bit samples (2^20 per channel, 10.9 s at 96 kHz,
in PSRAM): half the memory traffic of floats, so grains pitched up at
96 kHz don't overload the module. A frozen loop is saved on the SD card as
16 bit WAV (older 32 bit float files still load).

Grains share a budget: a new grain starts only while the playing grains'
read speeds plus its own stay within 28 (a grain at +24 st reads 4 samples
per sample, at -24 st 0.25, counted as at least 0.5): all seven grains
can play even at +24 st; only beyond that (pitch spray above +24) do they
get sparser, so the module never overloads. pollen runs with 128 sample
blocks (1.33 ms at 96 kHz): they spread the peaks of fast grain reads.
