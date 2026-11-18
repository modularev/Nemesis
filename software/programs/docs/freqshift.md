# freqshift

Stereo frequency shifter (Bode style) with spread, feedback delay, sideband outputs and a quadrature oscillator.

Source: [`freqshift.dsp`](../freqshift.dsp), version 0.2.

A frequency shifter moves every partial of the input by the same number of
Hz (unlike a pitch shifter, which multiplies them): small shifts give
phasing and beating, large ones inharmonic, metallic sounds. Through the
feedback delay every echo is shifted once more (spiralling echoes,
barber pole effects).

How: a pair of allpass chains splits the input into two signals 90 degrees
apart (an analytic signal, "Hilbert transform"); multiplied with a sine
and cosine at the shift frequency, their sum and difference are the input
shifted up and down.

```
in 1/2: stereo audio
out 1/2: main L/R (dry/wet mix)
out 3/4: shifted up L/R      out 5/6: shifted down L/R   (wet only, with feedback)
out 7/8: shift oscillator of the left channel, sine and cosine, +-4.75 V:
         a quadrature LFO at small shifts, an audio oscillator at large ones
```

```
hardware controls (rev2)                       rev1
shift, range, spread    X Y Z (knob + CV)       CV X Y Z
mix, delay              POT A, B (+ CV A, B)    CV A, B
feedback, sideband      POT C, D (+ CV C, D)    CV C, D
damp (feedback lowpass) setting (turn the encoder)
the home screen shows the shift of the left channel in Hz
```
