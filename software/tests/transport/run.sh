#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# builds and runs the transport core test on the Mac
cd "$(dirname "$0")" && c++ -std=c++17 -O1 -Wall -o "${TMPDIR:-/tmp}/nemesis_test_transport" test_transport.cpp && "${TMPDIR:-/tmp}/nemesis_test_transport"
