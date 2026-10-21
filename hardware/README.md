# Nemesis hardware

Files are sorted by hardware revision. Editable sources are DipTrace
(`.dch` schematic, `.dip` board) and Illustrator (`.ai`) files.

## rev1

The original module (2021): no display; X/Y/Z and A–F are read on Teensy
analog pins, each knob summed with its CV jack.

- **pcb** DipTrace schematics and boards (main, input and output module)
- **pdf** schematics as pdf
- **gerber** production files for the PCBs and PCB front panels
- **frontpanel** panel artwork (`euro_dsp*`) and front panel boards

## rev2

The 2022 redesign: 128x32 OLED, 7 CV inputs on an LTC1867 SPI ADC and
4 separate pots.

- **pcb** DipTrace schematics and boards (`nemesis-pcb_main-rev2*`,
  in/out modules and panels)
- **frontpanel** panel artwork (`nemesis_22*`, `nemesis_23`)
