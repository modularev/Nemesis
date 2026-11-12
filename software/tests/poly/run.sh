#!/bin/sh
# builds and runs test_poly.cpp (FAUST 2.88, Homebrew)
set -e
cd "$(dirname "$0")"
OUT=${TMPDIR:-/tmp}/nemesis-poly-test
mkdir -p "$OUT"
faust -cn junovoice -i ../../programs/juno.dsp -o "$OUT/junovoice.h"
faust -cn junoeffect -pn effect -i ../../programs/juno.dsp -o "$OUT/junoeffect.h"
c++ -O2 -std=c++17 -I"$OUT" -I../../faust -I/opt/homebrew/include test_poly.cpp -o "$OUT/test_poly"
"$OUT/test_poly"
