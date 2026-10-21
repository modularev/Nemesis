# template

Starting point for your own Nemesis program, see `../README.md` for details.

- **src/myProgram.dsp** the FAUST program, rename it and the `#include` and
  class name in `main.cpp` with it
- **src/main.cpp** starts the hardware and runs the program
- **platformio.ini** environments `rev1` and `rev2`, build with
  `pio run -e rev2 -t upload`
