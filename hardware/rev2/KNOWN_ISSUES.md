# rev2 known issues and ideas for the next revision

Measured on the rev2 test unit (main board v0.21) in November 2026 with an
HP 34401A, an ES-10 and loopback tests (`software/tests/cvtest`). Schematic:
`pcb/nemesis-pcb_main-rev2-dual.pdf`.

## Audio inputs 1–4 (and 5–6 in differential mode) can't measure DC

**What happens:** IN 1/2 are DC coupled up to the codec (inverting OPA1678
stage, `AIN1+ ≈ 2.5 V − 0.47 · Vin`), but with the codec high-pass frozen a
DC level arrives at only about 1/21 of the AC gain (steady, not decaying).
Audio from 10 Hz up is flat within 0.5 dB.

| codec mode (register 05h, high-pass frozen) | IN 1–4 DC / AC gain | IN 5–6 DC / AC gain |
| --- | --- | --- |
| differential (`0xC0`) | 0.0078 / 0.163 FS/V | 0.0078 / 0.163 FS/V |
| single ended (`0xDC`) | 0.016 / 0.326 FS/V | **0.326 / 0.326 FS/V, DC coupled** (offset −40…−60 mV) |

**Cause:** the negative inputs `AIN1−`…`AIN4−`, `AIN5B`, `AIN6B` (pins 45, 47,
49, 51, 57, 59) only go through 2.2 µF (C54–C59) to A_GND. At DC they float on
the codec's internal bias network and follow the positive input, so the
differential DC almost cancels. The CS42448 datasheet requires: "The negative
leg must be driven to the common mode of the ADC". In single ended mode ADC3
(IN 5/6) selects `AIN5A`/`AIN6A` through its multiplexer and doesn't use the
negative pins, which is why IN 5/6 then pass DC.

**Usable today:** IN 5/6 can be DC inputs with ADC3 single ended and its
high-pass frozen (register 05h = `0x44`: ADC1/2 stay differential), at the
cost of a ±3 V range on these two (measured with all three ADCs single ended, the combination `0x44` itself is untested).

**Fix for the next revision:** hold every `AINx−` at the common mode at DC,
e.g. a resistor (~10 k) from each `AINx−` to VQ (pin 43) or to a buffered
2.5 V matching the op-amp offset, keeping the 2.2 µF for AC. Then measure
IN 1–6 in differential mode (±6 V range) with the codec high-pass frozen and
calibrate offset and gain in software, like the outputs.

## Input range vs. ADC mode

The input stage maps ±5 V (schematic label "−5 to +5") to 2.5 V ± 2.35 V.

| codec mode (register 05h) | full scale at the jack, input pots fully clockwise |
| --- | --- |
| differential (`0x00`, used by the library) | ±6.1 V, ±5 V fits |
| single ended (`0x1C`) | ±3.07 V, ±5 V clips (single ended full scale is 2.8 Vpp, +6 dB digital gain) |

IN 1–6 agree within ±0.5 %. All 8 input stages invert; the library corrects
the polarity in software (`nemesis::inputScale`), since the CS5343 channels
can't be inverted in the codec.

Noise floor with nothing patched: −89…−94 dBFS RMS (IN 1–6), −75…−84 dBFS
(IN 7/8), unweighted.

## IN 7/8 (CS5343 on the AUX port)

- AC only: the CS5343 has a fixed 1 Hz high-pass.
- Full scale ±2.77 V at the jack, about 7 dB more sensitive than IN 1–6.
- If DC inputs are wanted on all channels, a codec without a fixed high-pass
  (or ADC3 of the CS42448) is needed for these two.

## MIDI input

Circuit: TRS jack → R61 200 Ω → optocoupler U7 (Everlight 6N137S1) → R69
200 Ω pull-up to +3V3 → Teensy pin 0 (RX1, the net is named `MIDI_TX`).
No bytes arrived from a MIDI keyboard on the test unit; USB MIDI works.

- **Supply out of spec:** the 6N137 is specified for VCC 4.5–5.5 V and runs
  on +3V3 here, which can leave its output stage from switching. Fix: VCC
  (pin 8) from +5 V, keep R69 to +3V3 (open collector output, the Teensy 4.1
  isn't 5 V tolerant), or use an opto specified for 3.3 V (e.g. TLP2361).
- **TRS type B:** tip → R61 → opto anode, ring → cathode, i.e. TRS MIDI type
  B. Most gear and adapters follow the MMA standard type A (tip = sink, ring
  = source). Type A works with a type A/B swap adapter, or a solder jumper
  for both types on the next revision.

## Outputs

- The output stages invert; the library compensates with the CS42448 DAC
  channel invert register.
- Uncorrected offset +43…+192 mV at 0, gain 4.95–5.02 V per unit. Both are
  calibrated per output in software (±0.4 mV against the HP 34401A); a
  hardware offset trim or matched references would make the module usable for
  CV without calibration.

## CV inputs

- X/Y/Z: main knob summed with the CV (through the attenuverter), ±4.2 V;
  their path is 4–5 mV nonlinear vs. ~1 mV for A–D (same ADC).
- A–D: 0…8.4 V, unipolar, readings below 0 V clip.
  On the test unit 0 V itself lies just below the ADC range on B, C and D
  (calibrated zero −0.003…−0.006 of full scale): unpatched they read the
  clipped end, 0.03–0.05 V after calibration. The library reads that end as
  0 V (`cvVolts`); 0…0.05 V can't be told apart on these inputs (V/Oct: up
  to ~0.6 semitones above C). `CAL` prints the calibration.
- The LTC1867L must run SPI mode 0 at ≤ 10 MHz with ≥ 5.7 µs per channel
  (datasheet limits); the original 24 MHz mode 1 timing caused a flat spot at
  mid scale (fixed in software).

## Controls

- Encoder A/B on pins 4/5 (clockwise counts down), switch on pin 6; not in
  the original pin documentation.
- Pot ADC noise: 24–33 LSB single reading; handled with 32x averaging and
  adaptive smoothing in software, a RC filter or a reference with less noise
  at the pot wipers would help.
- Pot noise source found (2026-11-17, `software/tests/adcnoise`, which now
  measures in passes): the audio clocking. Rewiring the pots to the
  Teensy's own 3.3 V and GND changed nothing. Pots in the middle, sd in
  12 bit LSB, single conversion / 32x averaging (the firmware's setting):
  Teensy alone (before `nemesis::begin()`) 33–43 / 2.0–2.6; with the
  firmware running 81–96 / 5.9–6.6; library pot interrupts off or display
  and its charge pump off: no change; SAI1 and MCLK (pin 23, 49.152 MHz) to
  the codec stopped: 4–6 / 0.9–1.0; interval timers (CV scan, MIDI poll)
  off: no further change. By position (firmware running): fully left 52–58
  / 3.5–3.9, middle 78–97 / 6–7, fully right 20–28 / 1.5–1.9, i.e. largest
  where the wiper's source impedance is highest: coupled pickup on the
  wiper-to-ADC path, not the pot supply. It aliases with the ADC timing
  (16x over 33 µs averages it out, 16x over 44 µs doesn't), as a clock
  asynchronous to the ADC's would. The audio pins already have slow slew
  (pad 0x10b0, drive strength 6); drive strength 3 gives 6.2, 1 gives 4.7
  (sd at 32x), weak for a 49 MHz clock: not used.
  **Next revision:** an RC right at each Teensy ADC pin (about 1 kΩ in
  series, 100 nF to the Teensy's GND), the pot traces away from MCLK, BCLK
  and the codec, a series resistor (22–47 Ω) at the MCLK source; or buffer
  the wipers as rev1 does (op amp, 33 kΩ ‖ 10 nF).
- Pot filtering in the firmware (2026-11-17, `nemesis_rev2.cpp`): the
  pickup also makes bursts of two conversions 50–160 LSB off; the median of
  3 let them through and the adaptive filter jumped with them (flicker).
  Now: median of 5, adaptive one-pole (rest 50 ms, full speed at 64 LSB
  difference), output in 4 LSB steps (1024) with 8 LSB hysteresis. Resting
  pots: 0 output changes in 60 s (filtered value within ~1 LSB); before up
  to 36 changes in 10 s. No knob popup for turns in the first 1.5 s after a
  start (X/Y/Z's CV ADC smoothing starts at 0).
- Display froze once (2026-11-17, turning POT A in harmonizer) while the
  sound, serial and the main loop went on (no reset, no late blocks): the
  SSD1306 probably took a disturbed I2C byte as a command and ignored the
  frames after it. The firmware now sends the controller's setup again every
  2 s (without display off). Cause on the board not known (I2C lines near
  the pots, no pull-up/ESD check yet).
- Pot spikes (2026-11-11, Teensy VIN from the bus +5 V): single
  conversions ~30 LSB off on POT D now and then (one per 30–60 s), which
  the adaptive smoothing passes as a turn and which opened the parameter
  popup (threshold 0.01 ≈ 40 LSB). A median of the last 3 conversions per
  pot removes them (max 4–10 LSB in 90 s). Not known whether the bus
  supply made them worse.

## Teensy 5 V supply

The 1117-5.0 that feeds the Teensy's VIN is too weak (2026-11-11). strike
(dense FPU code from ITCM at 816 MHz, the highest current draw so far) reset
the module after 5–30 s: reset cause 0x1 (power-on), no CrashReport. Just
before each reset all four pots read 6–7 % high together (the ADC reference,
the Teensy's 3.3 V, sagged). The display showed every pot-bound parameter
jumping, and pots that caught their setting wrote it, so a reset during an
SD write was likely (a damaged FAT on the card, 2026-11-10). With a larger
output capacitor it ran for minutes and then froze without a reset (the
watchdog didn't fire either): 3.30 V on the Teensy, 5.55 V on VIN.
Feeding VIN from the case's +5 V directly fixed it.

**Next revision:** a stronger 5 V regulator for the Teensy (switching from
+12 V, or the bus +5 V with a fuse/diode), with its output capacitor as its
datasheet asks (the 1117 needs a minimum ESR/capacitance). Check for a sag
under load: the pots all reading high at once in the `RAW` serial command.
