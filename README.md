# [WIP] Nemesis
8i/8o multichannel eurorack dsp platform based on teensy 4.1

**\hardware** panel, pcb and gerber files

**\software** testproject

**\doc** datasheets and additional information

**\media** 

### Specifications

- Cirrus CS42448 24 bit Audio Codec (6i/8o) with additional 2ch ADC PCM1808
- Teensy 4.1 (ARM Cortex-M7 at 600 MHz) with 512kb usable DMAMEM (can be extended with 2x ESP-PSRAM64 for additional 16mb EXTMEM)

- CV Inputs X Y Z with attenuverter
- inputs A-F configurable as CV <u>or</u> AC coupled audio inputs (passive, ch. 3-8)
- 2 ch audio in AC or DC coupled with up to 6db gain (full scale at 0dB 10vpp)
- 2 ch audio out AC or DC coupled (+-5v) 
- **input module:** 6 AC or DC coupled audio inputs with attenuators
- **output module:** 6 AC or DC coupled active audio outputs



note: audio inputs and outputs are BOTH inverted, which matters for DC

![Panel]()