#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Renders every Nemesis program on the Mac and checks its outputs
# (program_test.cpp: NaN, runaway feedback, silence, DC, clipping).
#
#   ./run.sh                  all programs of firmware/platformio.ini
#   ./run.sh juno pollen      some of them
#   SEED=3 ./run.sh           other random settings (default 1)
#   FAUST=/path/to/faust ./run.sh   another FAUST compiler (default: faust on PATH)
#   INPUTS=audio ./run.sh strike    audio even on trigger inputs (test_inputs "trig")
#   TRACE=1 ./run.sh pollen         every random set's peaks and settings
#
# With a built firmware (firmware/.pio/build/rev2/firmware.elf) it also lists
# the size of every program's compute(): above ~30 kB it no longer fits the
# Teensy's 32 kB instruction cache (codesize.py).
set -e
cd "$(dirname "$0")"
SW=$(cd ../.. && pwd)
FAUST=${FAUST:-faust}
SEED=${SEED:-1}
OUT=${TMPDIR:-/tmp}/nemesis-program-tests
mkdir -p "$OUT"
# the firmware's FAUST options (platformio.ini custom_faust_options)
FOPTS=$(sed -n 's/^custom_faust_options *= *//p' "$SW/firmware/platformio.ini")
# FAUST's headers from the compiler's own install, not /usr/local (an old 2.69)
FINC=$("$FAUST" -includedir)

if [ $# -gt 0 ]; then
    PROGRAMS="$*"
else
    PROGRAMS=$(sed -n 's|^ *\.\./programs/\(.*\)\.dsp *$|\1|p' "$SW/firmware/platformio.ini")
fi

"$FAUST" --version | head -1
FAILED=""
for P in $PROGRAMS; do
    DSP="$SW/programs/$P.dsp"
    DEFS=""
    "$FAUST" -lang cpp $FOPTS -cn Prog -I "$SW/programs" -I "$SW/faust" "$DSP" -o "$OUT/$P.h"
    NV=$(sed -n 's/.*\[nvoices:\([0-9]*\)\].*/\1/p' "$DSP" | head -1)
    if [ -n "$NV" ]; then
        "$FAUST" -lang cpp $FOPTS -cn ProgEffect -pn effect -I "$SW/programs" -I "$SW/faust" "$DSP" -o "$OUT/${P}_effect.h"
        DEFS="-DPROG_VOICES=$NV -DPROG_EFFECT_H=\"${P}_effect.h\""
    fi
    c++ -O2 -std=c++17 -w -I"$OUT" -I"$SW/faust" -I"$SW/lib/nemesis/src" -I"$FINC" \
        -DPROG_H="\"$P.h\"" $DEFS program_test.cpp -o "$OUT/$P"
    "$OUT/$P" "$P" "$SEED" || FAILED="$FAILED $P"
done

ELF="$SW/firmware/.pio/build/rev2/firmware.elf"
if [ -f "$ELF" ]; then
    python3 codesize.py "$ELF" "$SW/programs"
fi

if [ -n "$FAILED" ]; then
    echo "FAILED:$FAILED"
    exit 1
fi
echo "all programs passed"
