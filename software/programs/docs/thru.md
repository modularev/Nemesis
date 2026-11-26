# thru

Inputs 1-8 to outputs 1-8, unity: checks levels and the input and output scaling (5 V = 1.0).

Source: [`thru.dsp`](../thru.dsp), version 0.1.

Every input goes to the output with the same number, unchanged: no
controls, no output stage (no soft clipping), so what comes out is what the
input stage, the converters and the output calibration make of it.

```
in 1-8 -> out 1-8, 1:1
```

- IN 1-6: 5 V at the jack = 1.0 = 5 V out (with the input pot fully
  clockwise); up to +-6.1 V comes in, the outputs end a little above 5 V
  (the DAC's range after the calibration)
- IN 7/8 (CS5343): full scale +-2.77 V = 1.0 = 5 V out, i.e. 5.1 dB louder
  than IN 1-6
- IN 1-6 and IN 7/8 are AC coupled (IN 1-6 flat from 10 Hz, IN 7/8 from
  1 Hz): check with audio, not DC
- with the clock source on IN 8 (system setting), IN 8 is the clock and
  out 8 stays silent
