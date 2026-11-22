# Working on Nemesis

Teensy 4.1 Eurorack DSP module running FAUST programs. Hardware rev1 and
rev2 share one codebase (`software/`, overview in `software/README.md`).
Measured rev2 hardware facts and design issues: `hardware/rev2/KNOWN_ISSUES.md`.

## Tools on this Mac

- PlatformIO CLI: `~/.platformio/penv/bin/pio` (not on PATH).
- FAUST 2.88 from Homebrew (`/opt/homebrew/bin/faust`, first on PATH). An old
  2.69 install remains in `/usr/local`; FAUST searches install dirs before
  `-A`, so never give a repo header the same name as one in
  `/usr/local/include/faust`.
- Python tools: `software/tests/.venv/bin/python` (Python 3.12, pyserial,
  numpy, sounddevice, mido, python-rtmidi, hp34401a). Recreate with
  `software/tests/requirements.txt`. python-rtmidi has no 3.14 wheels.
- No `timeout` command on macOS: use tool timeouts and deadlines in Python.

## Devices

- rev2 test unit: `/dev/cu.usbmodem103226001`, USB MIDI port "Nemesis rev2"
  (software/firmware/src/name.c: "Nemesis rev1"/"rev2" by NEMESIS_HW_REV;
  macOS keeps a device's first MIDI name until its entry is removed in
  Audio MIDI Setup)
  when built with `-D USB_MIDI_SERIAL` (software/firmware).
- rev1 test unit (2026-11-12, first run of the unified code): runs juno
  (3 % idle, no late blocks), `/dev/cu.usbmodem78279001`, SD card installed (2026-11-18).
  Knob calibration in EEPROM (hold button 3 at start: min + button 1, max
  + button 2, button 3; a HANG/watchdog reset while holding works too):
  0.000 at min, 1.000 at max, noise < 0.001 p-p. CV V/Oct not calibrated.
  Remote page (2026-11-18): "Knob calibration (rev1)" section, serial KCAL
  [START|STORE|CANCEL|RESET]: sweep knobs, Store keeps each input that
  moved >= 100 readings (per input validity now; ends 2 readings inside).
  On 2026-11-18 its EEPROM held min ~ max (436-443: a button calibration
  without turning), i.e. uncalibrated. LEDs dimmed as on rev2 (4 kHz pulse
  density on pins 35-37, LED brightness setting).
  With both units on USB, unplug the other one for an upload (teensy-gui
  can't choose the board).
  Its PSRAM read bits wrong (2026-11-12, PSRAMTEST ~200 per 64 MB, bit 5
  or 1 of a byte read 1 = data line IO1; a flipped smalloc pattern crashed
  extmem_free() on a switch mooSpace2 -> mooSpace, CrashReport at
  smalloc_do_crash): the library clocked the PSRAM at 180 MHz (PLL3 PFD0
  720 / 4, comment said 133). At 88 MHz no errors; rev2's chip had none at
  180. Now 144 MHz (720 / 5; hw::setPSRAMspeed in nemesis.cpp, FlexSPI
  disabled and gated while changing). rev2 2026-11-17, mooSpace2 / pollen
  CPU: 180 MHz 77 / 78 %, 144 MHz 83 / 77 %, 132.9 MHz 86 / 87 % (mooSpace2
  peaks 98 %), PSRAMTEST 0 errors at all three. rev1 at 144 (2026-11-18,
  SD card in now): PSRAMTEST 0 errors in 6 x 4 x 4 MB, all 12 programs
  switch (mooSpace2 <-> mooSpace twice), CPU juno 3, mooSpace 46,
  mooSpace2 59-61 (max 72), pollen 64, strike 48 %, heap 433 kB after.
  Changing the clock while programs run hung the module. A PSRAM block
  tracker (check the pattern at stop, leak instead of crash) made rev2 hang
  ~1 s after starting mooSpace2 (watchdog; cause not found): removed.
- HP 34401A multimeter: `/dev/cu.usbserial-FTEJ7S3Y` (FTDI), RS-232 9600 8N2,
  HP-IB address must not be 31 (talk only). Driver and CLI:
  `~/GitHub/34401a-calibrator` (`hp34401a read -n 5`).
- Fireface UFX outputs 23–30 → ES-10 DC outputs (~11.05 V full scale, only
  handheld-meter accurate; the outputs calibrated against the HP are better).
- The user patches cables and turns knobs on request; say exactly what to
  patch and wait for confirmation.

## FAUST upstream work

- Fork ~/GitHub/faust (origin modularev/faust, upstream grame-cncm/faust),
  branch teensy-poly-memory on master-dev (2.94.1, 2026-11-19), pushed:
  faust/dsp/mcu-poly-dsp.h (= nemesis-poly.h, class mcu_dsp_poly),
  faust/dsp/tiered-memory-manager.h, faust/dsp/teensy-memory.h, teensy.cpp
  -mem and -nvoices without RTTI (upstream -nvoices didn't build on Teensy
  4: dynamic_cast), fprintf macro / MIDICTRL / destructor fixes; host tests
  tests/polyphonic-tests (make mcu-poly), tests/memory-manager-tests. All
  LGPL-2.1-or-later + FAUST exception. Not yet: running on a Teensy, a
  GRAME issue/PR (ask the user), Daisy tiers (user has a Daisy).
- PRs (2026-11-21, no Claude attribution): #1403 faust2teensy build fixes
  + MidiUI started on the first update() (a global object's MidiUI crashed
  before setup(): static init order; init_priority sections are dropped by
  the Teensy linker script; moved there from #1406 at sletz's request),
  #1404 mcu-poly-dsp.h (branch teensy-mcu-poly), #1405 tiered memory
  (teensy-tiered-memory), #1406 faust2teensy -mem/-nvoices
  (teensy-poly-memory-integration, on #1403's head). Daisy PRs later.
  #1404 review (sletz, 2026-11-22) addressed in 4fe2dfad2 and ported to
  nemesis-poly.h (dynamic arrays, re-steal/retrigger/last-note-wins, fade
  in after a cut, slices, soundfiles shared, all note zones); rev2 juno
  under MIDI stress: 0 dropped/NaN/late, 12 voices 70 % (peak 80).
  Open: a shared note-convention header with poly-dsp.h (sletz, optional).
  Hardware test of faust2teensy on rev2: scratchpad-style project with a
  stock Audio library sketch needs pin 9 HIGH (codec reset) and codec 03h =
  0xF5 (the library's 0xF4 left the outputs silent).
- Local master-dev compiler: ~/GitHub/faust/build/bin/faust (build/local,
  light backends; build/share/faust and build/include link the repo's
  libraries/architecture so -i inlines master-dev headers, not /usr/local's).
- 2.94 vs 2.88 on Nemesis (2026-11-18): all programs compile, but compute()
  grows (juno +13 %, mooSpace +19 %, mooSpace2 +11 % to 28 kB, superosc
  +22 %) (FAUST=~/GitHub/faust/build/bin/faust for faust_build.py).
  rev2 CPU 2026-11-19, 2.88 -> 2.94.1 (avg, 10 s each): mooSpace 57 -> 70,
  mooSpace2 67 -> 87 (peak 97, 1 late block), growler 55 -> 59, superosc
  25 -> 29, breaker 26 -> 30, others +-2: stay on 2.88. The growth is the
  libraries, not the compiler (2.94.1 with 2.88's libraries: sizes within
  +-5 %, juno +9 %); si.smooth's new imp(3) (stores the error and x', since
  2026-10-28) is part of it (growler, freqshift, bitrot back to 2.88 size
  with imp(0)); mooSpace(2), juno, superosc still +11..22 % from other
  library changes (de.fdelay now clamps d before int(): a candidate).
  mooSpace's CPU swings +-5-10 points with code placement alone (the
  fdelay clamp changes one line of its C++, yet 59 vs 68 %; old si.smooth
  smaller but slower): judge library changes by the whole picture.
  mooSpace2 (2026-11-19): nemesis.lib nm_smooth / nm_smoo / nm_fdelay (the
  2.88 forms, release independent) instead of si.smoo / de.fdelay: 2.94.1
  82-84 -> 62 %; LFO rate from the raw knob (no per-sample sin); declare
  options "[optimize:Os][faust:-mcd 0]": 20.4 kB, 50 % (peak 57, 60 s 0
  late, block 128); same source with 2.88: 54 % (was 67). Measured with
  a scratchpad script per variant: build, flash, PROG 3, 2 x 20 s.
  Upstream si.smoo/de.fdelay + Os + -mcd 0: 61 % (the nm_ forms still
  matter). All programs (2.94.1): -mcd 0 is neutral or 1-4 points better
  everywhere -> platformio.ini; Os everywhere made pollen 68 -> 74 and
  superosc 30 -> 32 % -> only mooSpace and mooSpace2 declare it. mooSpace
  with Os (14.1 kB) measured 52 or 59 % depending only on its address
  (cache offset 0x148c vs 0x3458): still placement sensitive, likely its
  per-sample libm calls through flash veneers (pow x7, log10 x6, tan,
  tanhf, sin: compressors and filters on smoothed values).
- Old 2022 fork branches (nemesis, custom-memory-manager, teensy-midi):
  deleted, kept in ~/GitHub/faust-2022-branches.bundle.

## Licenses (2026-11-19)

- SPDX line in every source file, `declare license` in programs; software
  GPL-3.0-or-later, juno.dsp / juno106_curves.lib / juno106_presets.py
  GPL-3.0-only (Ultramaster KR-106 is GPL v3 only), nemesis-poly.h
  LGPL-2.1-or-later + FAUST exception, fonts OFL-1.1; hardware stays
  CC BY-NC-SA 4.0 (the user's choice). README lists the exceptions.

## Spectral processing (2026-11-20)

- lib nemesis_spectral.h/.cpp: a phase vocoder for FAUST programs
  (`declare spectral "2"`, ffunction nm_spectral(ch, kernel, size, x,
  a..g) per sample, nm_spectral_level). 1024 or 2048 points, hop N/4,
  Hann; size changes are reallocated in the main loop (service()), never
  in the audio interrupt. CMSIS FFT set up by hand per size
  (arm_rfft_fast_init_f32 pulled in every size's tables: ~70 kB RAM1).
  CMSIS code and tables must stay in ITCM/RAM: from flash a 2048 frame
  took 150 % of a 64 block (tables) / 80 % (code). Frame functions run
  from flash (FLASHMEM, ITCM at 4 blocks); room made by moving
  readCommands, loop::service, buildItems, randomize to flash.
- FREEZE kernel: level history of 32 frames (PSRAM; capture swaps
  buffers: copying 66 kB was a late block = click), identity phase
  locking, tonal test per peak (stable turn + frequency within 0.6 bins,
  else random phase: unresolved partials played wrong tones, the
  user's buzz), level match (played energy -> input average) + output
  correction, drift = slow pitch wander per peak (random turns per frame
  buzzed), smear layer (levels and turns follow with inertia, tilt),
  time constants in seconds (setRate). Close low notes (< ~2 bins) can
  only freeze as texture; 4096 points would resolve them but doesn't
  fit RAM2 for stereo as laid out.
- slush (program 13): freeze/smear; block 128 (a 2048 frame with smear
  ~40 % of a 128 block, between frames 3 %). Mac tests: scratchpad st/
  (direct.cpp, bz/pc/lv/xf), tests/programs/run.sh slush. Not yet
  measured on rev2 with CMSIS back in ITCM; the user still heard a buzz
  (not reproduced on the Mac: ask for a recording of out 3).
- slush 0.4 (2026-11-21): morph (capture matches peaks to the nearest
  old ones within 0.5 oct, bends glide over the fade), layer (previous
  capture held, two layers normalised 1/sqrt(g0^2+g1^2); a sounding slot
  fades ~12 ms before a capture into it), grain (random phase +-grain^2 pi
  per tonal bin, output only), pitch (peaks move lrint(f(R-1)) bins, turn
  at f R via quarterTurns: polynomials, no libm; fastAtan2 < 2e-4 rad);
  smear runs with no inertia when off and pitch != 1 = live shifter.
  nm_spectral now takes a..k (15 arguments; the simulator's slush isn't
  synced). Smear rewritten as learning oscillators (the user's "peaks
  toggle", also before): a bin learns frequency + phase offset to its
  peak only when fresh (> 0.7 x smeared level, hysteresis 0.4), steady
  (level >= 0.95 x last frame) and tonal (tv smoothed, on 0.6 / off
  0.3), else runs on at its frequency; switching input phase / own turn
  per bin was the beads; learning in transition frames made tails take
  the next note's pitch. Level correction: planned (sum S.mag^2) vs
  played energy, 0.3 s / 0.5 s (chasing the input wobbled 2 dB at
  2-4 Hz). Pops (strong bins +6 dB over a +-8 frame median, scratchpad
  pops.py): piano 1.25 -> 0.44 %, juno 3.6 -> 2.0, church 4.0 -> 0.6;
  noise ~6 % by nature. rev2 48k 2048: 26.4 % (peak 31.5), pitch 27.8
  (34); captures with two layers peak 44-45 %. The user: smooth now.

## Switching, CPU meter, spectral fixes (2026-11-21)

- Program switch click: while no program writes (~7 ms) the DMA sent
  digital 0 and the DC calibrated outputs lost their offset (a DC pulse,
  measured). OutputTDM's ISR now fills each output's calibrated zero
  (outputCorrection offset x offsetGain). Clock changes (sample rate, block
  size): the offsets ramp to 0 (offsetGain, OFFSET_RAMP_S 60 ms, applied
  in nemesis_arch.cpp), DACs muted (no step at offset 0), clocks change,
  unmute 30 ms later, offsets ramp back, then the fade in. A DAC mute with
  the offsets applied was a low thump; muting while the codec's ramp still
  ran clicked. Codec 06h = 0x43 (soft ramp, no zero cross wait): 0x63 made
  quiet channels unmute slowly one after the other. After a clock change
  the ADCs ring up to full scale: inputs held at 0 for 30 ms + 5 ms fade
  (inputHold; mooSpace rang like a struck plate). `declare warmup "250"`:
  the fade in waits (mooSpace, mooSpace2: smoothed knobs glide from 0).
- Buffers read backwards / far back must not reach past what was recorded
  since the start (crackles while filling): mooSpace2 reverse (a piece
  plays only if REV_LEN were recorded, decided where its window is 0),
  pollen grains (age <= recorded).
- CPU meter: AudioProcessorUsage() is the last block only and its 16 bit
  count (cycles/64) wraps at 4.19M cycles. Now the program's whole update()
  is timed (32 bit, LoadTimer in nemesis_arch.cpp), blocks counted in the
  DMA ISR, 500 ms windows: cpuLoad() average, cpuLoadPeak() largest block
  (display shows the peak), report "CPU avg% (max peak%)", JSON cpuPeak.
  update_all() on Teensy 4 only sets the software interrupt pending: time
  around it measures nothing.
- slush/spectral: one 160 kB block per channel landed in PSRAM once RAM2
  was fragmented (after visiting programs); a 2048 frame there took 220 %
  of a 128 block (stutter on OUT 2, two channels = watchdog). Every array is
  now its own malloc in RAM2, never PSRAM (history stays PSRAM). Frames run
  in two halves (analysis, synthesis HOP/4 later, other blocks): peak 96k
  71 -> 41-45 % (freeze pressed: 105 % = click before), 48k 35 -> 21 %;
  wet latency + N/16.
- CAPTURE <input> <samples> / CAPDUMP: an input recorded from the DMA into
  PSRAM (patch an output into a free input); scratchpad-style analysis with
  numpy (spikes, levels). Input side rings at every clock change: a
  loopback can't judge the output at that moment.
- tests/programs/run.sh takes CXXFLAGS (e.g. -fsanitize=address -g).
- faust2teensy on stock Teensy sketches for rev2: pin 9 HIGH (codec reset),
  codec 03h = 0xF5.

## Simulator

- Nemesis Sim (AU/VST3 plugins that run the .dsp files in Ableton) lives in
  its own repo, `~/GitHub/nemesis-simulator`. The user asked not to work on
  it until they ask: don't change, build or sync it with library changes.

## Build, upload, debug

- `pio run -e rev2 -t upload` in a project dir; all projects use
  `upload_protocol = teensy-gui` (teensy-cli failed the first attempt almost
  always). After an upload the serial port appears after ~5–10 s.
- If the Teensy is gone from USB (crash loop), ask the user to press the
  program button, then upload once (bootloader shows as idProduct 1144 in
  `ioreg -p IOUSB -l`).
- Firmwares print `CrashReport` at start; decode the address with
  `arm-none-eabi-addr2line` from `~/.platformio/packages/toolchain-gccarmnoneeabi-teensy/bin`.
- Use short tool timeouts (upload ~60 s) and keep long hardware tests in
  short steps with progress output.
- software/firmware serial commands: `PROG <n>`, `CONTROLS`, `MEM` (PSRAM, 16 MB on the rev2 unit),
  `GET <path>` (parameter or meter, e.g. `/breaker/slice`), `STATUS` (MIDI bytes/USB messages,
  transport state, voice notes, NaN resets), CPU report every 2 s.
- `SCREEN` returns the display's framebuffer; `software/tools/screen.py
  out.png` saves it as a PNG (4x): look at the rev2 display remotely.
  Home screen top right (2026-11-17): the CPU load as a number ("64%") in
  the built-in 5 x 7 font (the user's choice over "cpu 9%" and a bar);
  CLIP / limiter readouts take its place as before.
  Display refresh (2026-11-17): redraw every 25 ms, the frame sent only when
  it changed (pushDisplay: FNV checksum of the 512 bytes, at least once a
  second); the CPU number updates every 500 ms (changing every frame it
  forced every transfer). breaker 35 frames/s at 3.4 ms avg (was 8.9 at
  12.3 ms: the beat count skipped sixteenths). I2C at 1 MHz broke the
  transfers (frames up to 358 ms): stays at the library's 400 kHz. STATUS
  prints frames/s and ms per frame.
- Button LEDs (2026-11-17, rev2): dimmed by pulse density from a 4 kHz
  IntervalTimer (pin 34 has no hardware PWM; integer accumulators, fixed
  pins; priority 64, above the audio so they don't stall). A 16 kHz float
  version cost mooSpace2 ~3-4 points. The loop shapes on/off: 5 ms up,
  30 ms exponential decay (80 stayed lit too long), gamma 2.2; "LED brightness"
  1..10 = 3..100 % duty (lower flickers at 4 kHz), off below 2 %; SD key
  led_brightness, remote SYS ledBrightness. Default 5 (the user likes it).
- Home layout (2026-11-17): `declare home "A|B";` in a .dsp (Program::
  homeLayout, rev2 drawLayout): up to 3 values big (bargraph, control or
  setting labels, @tempo, @beat), name in the top line; `[digits:N]` for
  decimals. Chosen over a separate layout file (second format and parser,
  drifts from the .dsp) and over splitting .dsp into audio + bindings (two
  files per program, and the simulator compiles the .dsp files).
- Program descriptions (2026-11-17): in software/programs/docs/<name>.md
  (controls, modes, background); a .dsp keeps its declares and one line
  pointing there. Write new programs that way; implementation notes stay
  as comments next to the code they explain.
- System setting "knobs" (2026-11-17): catch (a knob takes over a stored
  value when it reaches it) or jump (when moved 1 % from where it was when
  the value came: Control.jumpRef, catches() in nemesis.cpp); SD key
  knob_jump, remote SYS knobJump. The user wants jump (set on rev2).
  rev1 always jumps (2026-11-18, the user; no display to show a pending
  catch), the page hides the setting there. rev1 knob layout (2026-11-18):
  A-D as rev2's POT A-D in every program, extras on E/F.
- Remote control (2026-11-12): software/tools/remote/index.html (Web Serial,
  Chrome/Edge, file:// works) polls `JSON` (one line: programs, current
  program's controls, system, transport) and sends `PROG <n>`, `PSET <index>
  <value>` (settings only, not knobs), `SYS <name> <value>`
  (lib/nemesis/src/nemesis_remote.cpp; knobs take a PSET value until they
  reach it, as with presets). It holds the serial port: close it before
  uploading. Patches: the module's SD list, or a .PRE file loaded in the page
  (kept in the browser; software/programs/juno.PRE, from juno106_presets.py) sent value by
  value; rev1 pots are inputs, so their patch values show as meter markers.
  The user didn't want presets built into the firmware.
- firmware/upload_guard.py stops an upload with two Teensys on USB or when
  the connected unit's USB name ("Nemesis revN") doesn't match the build
  (a rev2 build once went to rev1).
- The transport PLL (`lib/nemesis/src/nemesis_transport_core.h`) is plain C++: test changes on
  the Mac with a host harness before flashing.
- Program tests (2026-11-20): `software/tests/programs/run.sh` renders every
  program on the Mac (NaN, runaway, loudness, DC, culprit search, compute()
  sizes); run it after changing a program or the FAUST version.
  `module.py` measures all programs on a module against
  baseline_rev<N>.json (no baseline saved yet). INPUTS=audio (audio into
  trigger inputs), TRACE=1 (every random set). Found and fixed
  (2026-11-20): strike with audio into its triggers at max settings grew
  modes until inf/NaN (hits in phase with a mode: resonance) -> resonator
  state clamped +-8 (RES_MAX), voice outs 3-8 soft clipped like the mix;
  pollen out 7/8 burst to 52 when tone jumped (fi.lowpass/highpass biquads
  with a sudden cutoff change) -> fi.svf.lp/hp (same Butterworth response,
  cutoffs still per block). CPU of both not yet measured on the module.
- tests/hwtest serial commands: see the top of `software/tests/hwtest/src/main.cpp`
  (DC/SINE/FREQ per output, AIN meters, REG codec write, CVCAL/OUTCAL/CALSTORE).

## Things that bit us

- FAUST `attach(0)` means `attach(_, 0)` (last argument first): use
  `(0, _) : attach` to keep a meter without outputting it.
- With `-mem` the generated `init()` is empty: call `classInit()` and
  `instanceInit()`.
- No FAUST MidiUI/poly-dsp.h (2026-11-11): they kept MIDI programs
  allocated forever (juno ~230 kB heap: every FAUST dsp_voice is a MapUI) and
  needed RTTI. The library maps [midi:..] like MidiUI, nemesis-poly.h runs
  [nvoices:N] (skips silent voices, frees with the program; stealing fades
  3 ms out/in: one block of gate off clicked audibly). nemesis-poly.h is a
  FAUST 'midi' receiver like poly-dsp.h (registers with a MidiUI, channels,
  CC 120/123, Panic, Polyphonic/Voices paths; a held note played again
  retriggers its voice, setRetrigger(false) = poly-dsp.h);
  software/tests/poly/run.sh checks it against poly-dsp.h on the Mac: same
  paths, MIDI, bit identical audio (noteHz in double, as poly-dsp.h; never
  name a constant VOICE_STOP_LEVEL: poly-dsp.h #defines it). Test commands
  MIDICH <n> (channel, not saved; the sequencer on the jack plays on several
  channels).
- `ve.moog_vcf` is only valid below SR/6.3; it blew up juno. `ve.moogLadder`
  is stable to 20 kHz.
- `AudioMemory()` needs a compile time constant (`NEMESIS_AUDIO_MEMORY`).
- The flash linker script must discard `.ARM.exidx` of the code it moves to
  flash (31 bit offsets can't reach from ITCM).
- rev2 CV jacks A–D have no knobs: bind parameters to a POT plus `[cv:CV_x]`,
  an unpatched jack reads 0.
- CPU: Mac benchmarks of FAUST code don't predict the Teensy (PSRAM reads,
  memory placement): measure on the module (PROG <n>, CPU report). A program
  that needs > 100 % starves loop(): no serial output at all. FAUST evaluates
  pow/tan/log of smoothed knobs on every sample: compute from the raw slider
  and smooth the result. Gliding and crossfading heads side by side on every
  mooSpace delay overloaded; one kind of head with motion blending lengths
  costs ~1.7 % per delay line.
- Code size: FAUST programs run from flash through the 32 kB instruction
  cache. A compute() bigger than that (strike 0.4: 40 kB) slows down far
  more than the Mac predicts (57 -> 85 %): check with arm-none-eabi-nm -S
  on firmware.elf. `declare options "[itcm:on]"` puts it into ITCM (RAM1,
  32 kB blocks; strike: 49 % flat). ITCM must stay at an even number of
  blocks: with strike it grew to 5 (_itcm_block_count, _flexram_bank_config
  aaaaabff in firmware.elf) and mooSpace2, which doesn't even use ITCM
  code, went from 74 to 90 % (max 108) - DTCM (arena, stack) with an odd
  bank count is slower. SD, SerialFlash and Adafruit libraries now run
  from flash (teensy41_flash.ld): 4 blocks, stack 64 kB again, 74 %. -vec (vector
  mode) made it 104 kB and overflowed the stack (crash loop: program
  button, then upload); [optimize:Os] saved only 4 %. FAUST also drops
  sliders whose value is unused (attach them) and sorts sliders
  alphabetically ("[01] name" orders them).
- Power: the rev2 1117-5.0 for the Teensy's VIN was too weak: strike's
  dense compute() from ITCM at 816 MHz reset the module (cause 0x1, no
  CrashReport) or froze it without a watchdog reset; all pots reading high
  together just before (`RAW` command) = the 3.3 V sagged. VIN now from the
  case's +5 V (2026-11-11), see hardware/rev2/KNOWN_ISSUES.md. CPU % doesn't
  predict current: memory stalls draw less than steady FPU work. Reset
  causes in the report: 0x1 power, 0x80 watchdog. `declare cpu_mhz "600"`
  lowers the clock per program (unused now).
- Heap (RAM2) ran out after visiting every program (2026-11-11): starting
  strike crashed in FAUST's MapUI (operator new returned null, a std::map
  insert, CrashReport at address 0x10, then a watchdog reset). MapUI kept
  maps of every path/label/short name of every running program (strike
  ~60 kB) only for the serial GET; it's gone (NemesisFindUI walks the UI
  when asked). Heap free after boot 466 kB (was 215); every program is
  freed now (vectors swapped empty, MIDI programs too): ~462 kB after
  switching through all. Check with MEM after switching through all
  programs when adding one.
- A damaged FAT (a cluster chain that loops, after resets during SD writes)
  made a settings file read forever: loop() stalled, the module reset
  without a CrashReport and booted into the same program again (strike
  "crashed while randomizing", 2026-11-10). Now: settings files are read
  at most 32 kB (more: renamed to BAD_<name>, never deleted, or the card is
  left alone), written to <name>.NEW and renamed when complete, an RTWDOG
  (WDOG3, 4 s, fed in loop() and long SD/calibration work) resets a stalled
  loop (reset cause 0x80), and starts that end before 10 s are counted in
  EEPROM (1030/1031): after 2 the next start is program 1 with the SD card
  off (home screen "SD off", STATUS shows it). The current program is
  remembered only after 10 s. The SNVS_LPGPR registers did not keep values.
  Test commands: SET, RANDOM, ROLL, MUTE/UNMUTE, SILENT/LOUD, CLOCK <MHz>, RAW,
  HANG (watchdog test).
- FAUST delay memories (2026-11-17): a recursion inside another one, or
  a recursive signal fed back as a second output of the outer ~ and read
  with @, made FAUST build the same 2 MB delay memory 2-19 times (count
  the [524287] arrays in the generated code). One recursive signal whose
  feedback block does every delayed read (they read d - 1: ~ adds one
  sample), outputs reading the same signal outside, gives one. Selecting
  outputs of a with-local block several times (b : (_, !, !)) copies it:
  route once (process = _, _ <: _, _, (f <: _, g) : out). `outputs`,
  `inputs` and `control` are FAUST keywords (syntax error as a name); a
  bargraph can't share its label with a slider ("path already used"). A
  float->int delay index needs max(0, ...) after int() (overflow).
- Test firmwares that call nemesis::begin() get the 4 s watchdog and the
  failed-start counter: tests/adcnoise didn't feed it and restarted every
  ~4.7 s (2026-11-17, fixed: watchdogFeed() in its waits and loops), and
  its 40 short starts put the real firmware into safe start (program 1, SD
  off) until one start ran 10 s. After many watchdog resets the SD card
  read "no SD" once (it keeps power); the next restart found it again
  (reproduced twice: the first start after a safe start): settings::begin
  now tries SD.begin a second time after 100 ms; checked by forcing a safe
  start (3x HANG before 10 s) and restarting: card found at once.
  rev2 pot noise is the audio clocking (MCLK/SAI to the codec), see
  hardware/rev2/KNOWN_ISSUES.md; tests/adcnoise measures in passes.
  Pots A-D (2026-11-17): median of 5, rest 50 ms, snap 64 LSB, 1024 steps
  with 2-step hysteresis: 0 changes in 60 s at rest (tuned with RAW and a
  temporary print of the filtered value: the flicker was bursts passing
  the median of 3, not noise). Popup ignores turns for 1.5 s after a start.
  Display froze once with the firmware running (turning POT A): rev2
  service() resends the SSD1306 setup every 2 s (refreshDisplaySetup:
  begin()'s sequence without display off, contrast via applyBrightness).
  To catch a test's output after an upload, reconnect on every port drop
  until it prints (the port comes and goes several times).
- Per-sample libm calls (2026-11-17): mooSpace2 rose from 83 to 93-98 %
  with no change of its own: fmodf (nm_position in nemesis.lib, its
  shimmer phase, a float % of REV_LEN / 2) ran per sample; libm sits in
  ITCM but compute() reaches it through veneers in flash, and the fmodf
  veneer's cache line landed in the range compute() (25 kB, 2-way 32 kB
  I-cache) already filled twice: evictions on every sample after the
  firmware grew. Now nm_wrapped(x, n) = x - n*floor(x/n) (floor inlines
  as vrintm): 75-81 %, breaker 25 -> 22, pollen 77.7 -> 75.3. Check sample
  loops with objdump for bl <__*_veneer> (per-block calls are fine; a
  user function used with _ needs \(v).(f(v, n))). The pot ADC interrupt
  was priority 192 (above the audio update, 208); now 224.
- Single slow blocks (2026-11-17): serial BLOCKS logs the cycles of
  compute() for 4096 blocks. mooSpace2 at 64 samples: median 75 %, but
  irregular single blocks up to 118 % (gaps 0.02-3 s, not periodic; not
  the display: MONITOR 0, not the pot interrupt, not drag) -> ~1 late
  block (click) per 7 s. Likely its modulated PSRAM heads colliding in
  the 32 kB data cache now and then. declare block_size "128": 0 late in
  60 s, max 89 %. juno idle shows no spikes (too short to be hit).
- RAM2 tier in the DSP memory manager (2026-11-17, nemesis_arch.cpp):
  heap < 1 kB, arena (DTCM) <= 32 kB, then buffers <= 128 kB the arena
  can't take from the heap in RAM2 (on chip) while 64 kB of it stays free
  (nemesis::heapFree), the rest PSRAM. mooSpace2's late taps and tank
  delays (106, 72, 3 x 65.5 kB) left PSRAM: 74 -> 63 % (max 89 -> 75),
  mooSpace 54 -> 50.5 %, others unchanged; heap after visiting all
  programs 427 kB (fragmented: 48 kB at the top, larger buffers then go
  to PSRAM). mooSpace2 at 64 samples now 3 late blocks in 70 s (was 11
  in 80 s): it keeps block_size 128 (0 late).
- pollen at 96 kHz, pitch +24 st (2026-11-17): 97 % (max 112), late
  blocks: memory bound, each voice reads its PSRAM buffer 4x faster
  (~770k cache line fills per second). Idle voices kept running through
  PSRAM (t counted on after the grain): position stopped at the grain's
  end (te = min(t, S); the window is 0 there): +24 st 77-79 % (max
  87-90), density 1.0 77-82 % (max 93), 0 late. Size 1 + density 1 +
  +24 st still overloaded (watchdog reset). Now: 16 bit memory (declare
  mem16 "2"; lib nemesis_mem16.h: inline nm16_put/nm16_get through
  ffunction "nemesis_mem16.h" (no <>: FAUST writes the string into the
  #include), get takes the put's value so FAUST orders them; buffers
  allocated/freed by the architecture at start/stop, no PSRAM -> the
  program doesn't start) and a grain budget (BUDGET 20: sum of playing
  grains' read speeds, min 0.5 each; grain_bank feeds the load back).
  Worst case 77-83 % (max 85), 0 late; -24 st 61 %. Mac render vs the
  float version: -60..-76 dB difference. Loop save: 16 bit PCM WAV, float
  WAVs still load (nemesis_loop.cpp Memory); saved/loaded on rev2.
  Then BUDGET 28 (all 7 grains at +24 st) with declare block_size "128"
  (64 peaked at 95 % with budget 24): worst case 76-80 %, peak 89.6 %, 0
  late in 30 s (23301 blocks).
  Sync density (2026-11-17, the user: too sparse at the top): 1 bar ..
  1/128 (ticks 96 .. 3, 1.5, 0.75; fractional ticks work with nm_grid);
  Mac: 8/32/64 grains/s at 0.6/0.8/0.95 (120 BPM); rev2 sync 1/128 +
  size 1 + +24 st 60-66 %, peak 88 %, 0 late.
  Size up to the memory length (was 20 ms .. 2 s): S_raw from mem_len
  (the setting, block rate: a pow of L would run per sample), then <=
  L / (1 + ratio). Memory 10 s, size 1, density 1: 60-66 %, peak 74 %,
  0 late at -24/0/+24 st. Open: ratio = 2^(semis/12) is a pow per sample
  (pitch spray draws per sample), through a flash veneer.
  Looper (2026-11-17): loop out (the frozen memory's previous pass, w(t-L),
  part of the wet signal: wet = grains + level * loop, mix fades input vs
  wet (the user, 2026-11-17), also on 7/8), memory in bars with sync
  ("memory bars [name:memory] [if:sync]", halved while > MAXL), freeze
  quantized to the next bar line with sync + running transport (freeze =
  sAndH(nm_grid(96), freeze_raw)), overdub + loop feedback in live().
  Mac: loop starts at the bar line, repeats exactly, content = the bar
  before. rev2 frozen with loop out + overdub 65 % (max 76). Overdubs
  after the freeze aren't saved to SD (saved once per freeze).
  Overdub with feedback 1 built the loop up to the 16 bit limit (hard
  clipping recorded for good: 78k of 96k samples clipped after 60 s in a
  Mac render with input in time with the loop; measure on out 7/8, out 1/2
  go through the output limiter and hid it): the write is nm_softclip(1.0)
  now (peak 0.983, nothing at the limit).
- Start and switch clicks (2026-11-17): hw::begin's codec.volume(1.0)
  unmuted the DACs right after enable() while the program played at full
  level: now outputGain/outputTarget 0 at begin, DACs muted again (07h FF)
  and unmuted START_MUTE_MS = 400 ms later in update() (soft ramp, 06h
  0x63 from the Audio library), then the program fades in. Switch fades
  15 ms out / 30 ms in (were 5/10; the outputs are DC coupled). Not
  checked by ear yet.
- Program switching (stop/start): every program is freed (since
  2026-11-11, also MIDI ones). start() and stop() run with
  AudioNoInterrupts and update() waits for fReady.

## State (2026-11-03)

- Calibration in the rev2 test unit's EEPROM: outputs vs. HP 34401A (±0.4 mV),
  CV inputs in loopback from them (±1 mV A–D).
- Presets (2026-11-11): a setting with [action:preset] chooses a line of
  /NEMESIS/<program>.PRE ("name|label=value|..."), names become its menu,
  a choice sets the values (knobs catch them); the stored choice isn't
  applied again at start. juno.PRE: the Juno-106 factory patches from the
  user's editor library (~/Downloads/Juno-106-Factory.106, Java serialized),
  made by software/tools/juno106_presets.py. Serial WRITE <path> <bytes>
  puts a file on the SD card.
- Encoder (2026-11-12, the user's choice): home turn = [home:1] setting
  (juno: patch, its name big; programs without one: opens the settings),
  click = settings, hold = program list, hold in the list = system
  settings (also the settings list's last row "system >").
- Firmware programs: juno (0.17, 2026-11-12: pulse in phase with the saw
  (the 106's comparator; inverted it partly cancelled: A15 9 dB quiet),
  oscillator mix saw 0.6 / pulse 0.5 / sub 0.75 / noise 1.2 as Youth106,
  output -16 dB (chords were +10 dBFS median) + Volume setting (dB), keys
  setting poly 1 / poly 2 / unison ([poly:mode] in nemesis-poly.h; a voice
  fading after Panic/CC 120/mode change takes a new note, else notes were
  dropped for 3 ms). The user's MIDI was an octave low: with it fixed juno
  is within +-0.1 oct of the recording for 6 of 8 patches (A15/A16 +0.2/
  +0.3), as close as Youth106 itself (rendered locally: scratchpad
  yrender, its src/dsp with 4 lines rewritten for the old Rust 2022 in
  ~/.rustup; ~/.cargo/bin links are broken);
  0.16, 2026-11-12: the 106's measured curves from
  Ultramaster KR-106 via Youth106 (github.com/nagualcode/youth106, GPL-3):
  programs/juno106_curves.lib (cutoff 5.7 Hz-50 kHz over the slider, env /
  LFO / key follow added in slider units as the firmware, attack and
  decay/release times, LFO rate/delay, DCO LFO depth, sub, noise),
  resonance curve + peak pull, HPF 236/754 Hz + the bass boost circuit,
  chorus 3.3 +- 2.13 ms, dry 0.863 / wet 1.257, VCA level +-10 dB. The
  0.15 fit was wrong: the user's .106 library had A11 and B31 swapped by my
  alias guess ("Brass Set 1" is A11). The converter now matches Youth106's
  factory banks value for value (PWM /105, DCO LFO taper). Against the
  user's recording of A11-A18 (Desktop, played from "juno chords.mid" at
  ~1.6x): 0.46-0.6 oct darker (A11/A12 1.2-1.3), cause open. FAUST 2.88
  bug: a table read while generating another table stays 0 (cutoff curve
  now a degree 9 polynomial). Curves from raw sliders, smoothed after:
  72 % with 12 voices at 48k, 76 % with 6 at 96k;
  0.15: fitted to a recording of A11 (the user's
  ~/Desktop/juno-a11.wav, chords rendered locally with FAUST and compared
  by spectral centroid): RC envelope (linear attack, exponential decay and
  release; slider time = 2 time constants), cutoff slider 0 = 113 Hz at C4;
  brightness curve within 0.28 oct (first 0.2 s still brighter: the 106's
  attack at slider 3 seems slower). Presets in factory order A11..B88
  (names from synthmania's factory recordings, juno106_presets.py);
  0.14, from the Juno-106 service notes 1984:
  filter 5 Hz-50 kHz, env +14 oct, LFO VCF +-3.5 oct, DCO LFO +-400 cents;
  the FREQ slider spans 248 Hz at C4 (calibration: C4 -> 248 Hz) .. 50 kHz,
  only modulation reaches 5 Hz (slider 0 = 5 Hz made the factory brass
  plucky/silent); envelope 1.5 ms + max * v^3 (exponential made decay
  44/127 34 ms; the curve is an assumption). All but 2 presets reach the
  level target now (before: 7-9 patches with closed filters stayed quiet);
  0.12: X/Y/Z cutoff/resonance/env, POT A-D
  attack/decay/sustain/release, LFO rate and PWM settings; limiter removed
  (the user found it sounded bad); 0.11, from the Juno-106 owner's manual: LFO
  0.1-30 Hz exponential, HPF a menu 0 boost / 1 flat / 2 / 3 (240/720 Hz
  and the +6 dB shelf are estimates), envelope end points as the manual
  (curve between: exponential, a guess); self-oscillation started by a 5 ms
  noise burst x resonance^4 (12 factory patches use it); CPU 72 % with 12
  voices at 48k, 77 % with 6 at 96k (0.9/0.10's patch parameters ~ +7);
  0.10: pitch bend +-2 st, chorus a menu setting
  (Z free), stereo peak limiter after the chorus (-1 dBFS, 150 ms), env
  depth = full cutoff range (11.4 oct, 6 left factory brass dark), note
  start kicks a resonant filter (resonance^4), Level 0..2; presets levelled
  by software/tools/juno_levels.cpp (loudest 50 ms of C4 rendered on the
  Mac, target rms 0.12; 8 noise/percussion patches stay quieter, Resonance
  Funk ~silent: decay 0.15 = 6 ms on the exponential scale); 0.9: LFO
  delay, level, range, PWM mode, env polarity, VCA env/gate, patch (listed
  first: library puts [action:preset] at the top); exponential envelope
  times like the 106;
  0.8, 2026-11-11: setting bass comp (feedback
  of y - c x: passband (1 + kc)/(1 + k); 1 keeps the bass at any resonance,
  0 lost up to 13.6 dB); drift setting (per voice
  detune, per note, slow wander; the filter drifts in cents like the DCOs, in
  octaves it wasn't playable when self-oscillating), cutoff 20 Hz-54 kHz
  (table to 0.45 SR), self-oscillates (k 4.3, saturation at 1.0: louder than
  a note, -80 dB noise); shared controls smoothed per voice but taken at once
  at a note start (an idle voice glided from its last note's cutoff), chorus
  wet path 10 kHz 12 dB LPF; fixed pan per voice via [poly:voice], least
  recently used free voice; 12 voices at 48 kHz 63 %, 6 at 96 kHz 67 %
  (0.5 without drift: 54/57 %), idle 3.5 %; one polyBLEP ramp for saw/pulse + half-rate ramp for
  the sub, TPT ladder with its coefficient from a table and a soft saturated
  input instead of ve.moogLadder's pow/tan per sample, HPF global in the
  effect; was 6 voices 54 %), mooSpace,
  breaker (beat slice repeat/reverse, crush, comb, drive, drum triggers on
  outs 3–8 from a pattern map; on the transport),
  freqshift, superosc, growler (stereo 4-op FM bass, keys/V-Oct play the operators, 27 %); 816 MHz, 48 kHz.
- mooSpace2 (program 3): v1 algorithm with wider controls; smear = diffuser
  strength (blur), drag = diffuser lengths + late taps (time stretch; was
  smear's job, which made smear sound like a delay at short decays); tone (POT C) in
  the tank replaces colour + filter, reverse (POT A), shimmer (POT B, pitch
  shifter where input and feedback enter each tank half, so it works at
  short decays too; modulate is a setting now; tank compressors replaced by
  soft limiters (they pumped ~2 Hz at high decay + loud input), one fixed
  allpass more per tank half (1103/1213); held tones still beat a few dB in
  long tails, v1 too: fixed tank resonances, modulation didn't help);
  smear: input diffuser coefficients capped at 0.6 (0.85 let 52 % of a
  click through: allpasses pass -coeff at once), two more fixed integer
  input allpasses 397/491/607/743 (gliding ones cost +9 %); the 4 input
  diffusers use the moving heads too (motion 0 = no pitch when turning
  drag/smear, +5 %), paid for by a soft clip instead of the input's 1176
  limiter + tanh: 67 % (max ~80); lag's predelay still glides (2 heads on
  its PSRAM buffer cost ~6 %);
  v2.8: lag in ms or note values (library [name:]/[if:]/[ifnot:]),
  motion on/off, crossfade fixed 25 ms, MAX_LAG 2^16;
  v2.7 (2026-11-09): freeze (gain 1.004: interpolating reads lose treble),
  ping-pong echoes from the reverse buffers (4 PSRAM reads), duck, clocked
  gate, lofi in the feedback (needs a low-pass after the hold: its images
  made long tails grow), 8 outputs (3/4 rear, 5/6 wet, 7/8 envelopes);
  CPU 74 % (max 90-95, 0 late of 42029); was 67 %. The crossfade heads had
  swapped take triggers (jumps while audible), fixed;
  decay knob = tail time, RT60 0.4..60 s exponential to 0.95, gain up to
  1.05 above (was 1.1*sqrt(knob): 1.5-4 s over the first half, endless
  from 0.83); at decay 0 the tail is smear/drag's (~1.5 s at defaults);
  sync setting (2026-11-09): lag on 1/16, drag's late tap on 1/32, reverse
  pieces a note value locked to nm_position (buffer 2 x 65000, PSRAM);
  checked on the Mac, CPU on the module unchanged with sync off (66-68 %);
  block 64: 67-70 % (max
  77-84 %, 111 % only in the switch report); the shimmer costs ~5-8 points
  (its 2 x 32 kB delays don't fit the arena any more). Motion on the tank and
  input diffuser delays. Block size setting is not stored (auto at boot). mooSpace v1
  56 % since the RAM arena. freqshift 13 %, superosc 25 %, breaker 14 %.
- pollen (program 8, 2026-11-10, prototype, uncommitted): live granulator,
  loop memory w = in*(1-f) + w(t-L)*f (freeze f -> 1), grain start as age
  behind the freeze point (else frozen grains read across the seam), 8
  voices, 2 x 2^19 buffers shared by loop and grain reads (PSRAM); 30 % CPU
  (max 34); Mac: pitch +/-12 exact fwd/rev, freeze holds level, sync 1/8 ok.
- strike (program 9, 2026-11-10, uncommitted): 6 voice modal percussion,
  triggers IN 1-6; MIDI via the library ([mono:trig] [chan:N] / [note:N]):
  voice per channel base..base+5 (note transposes, C4 = set pitch) or notes
  36-41 (no FAUST MidiUI any more: a normal program, freed on switch). A
  per-trigger (per-sample) transpose put every mode on per-sample
  coefficients: 78 % (max 92); control rate (the note zone): 54 % (max 65).
  0.3: pager (button 1, release), audition (button 2), LEDs, sections,
  numbered labels ("[01] v%v type": FAUST sorts sliders alphabetically
  otherwise). The edit knobs' sliders were unused and FAUST dropped them
  (POT A-D unbound since 0.1): attached now. 57-67 % CPU. Library: knob
  catch with saved values for all programs (rev2). Not yet tried by hand.
  0.4: randomize (voice/all, amount; library [action:randomize|amount],
  [random:0]), per voice tension, dynamics, variation, rattle; modes per
  slot 4,5,4,6,6,4; compute() in ITCM: 49 % (max 50).
  0.5: rolling instead of the randomize menu (library [action:roll|target|
  amount]: history of 16, turn right = roll, left = back; button 1 held +
  button 2 = roll + play the voice). Edit knobs that had caught their
  setting kept writing it and overwrote rolls: they now let go when the
  target changes elsewhere (Control.editSet). Serial test: ROLL +|-.
  0.6: mix renamed noise blend; X/Y/Z assignable macros for all voices
  (settings x/y/z, [title:x] popup), POT A-D inharmonic/position/bend/
  tension; rolls skip settings held by edit knobs (every page) and by
  routed CVs with a signal (Program::held). 47 % CPU.
- mooSpace2 at 88-94 % on 2026-11-10 was the 5 ITCM blocks (above), not
  its settings: the pushed firmware (d59b2e9) gave 73 % with the same
  settings, the current one without strike in ITCM 74 %. Per voice settings shown by
  "voice" ([if:voice=N]), POT A-D [edit:v{voice} ...]. CPU: 8 modes with
  per-sample coefficients for all overloaded (silent serial; the program
  isn't stored then, a reboot comes back); 8 modes, 3 bent: 75 % (max 84);
  6 modes, 2 bent, block 64: 55 % (max 66). Mac ratio for it ~60x.
  Settings list and edit knobs not yet looked at on the display.
- bitrot (program 10, 2026-11-14, new, uncommitted): data error/glitch
  buffer (genre of a circuit-bent buffer; the user's reference was a
  commercial module, keep it original). One delay line per channel (2^19,
  PSRAM) read at a computed delay with linear interpolation serves every
  error type; freeze recirculates at the event length held when freezing.
  Dice = integer hash of event number (synced: nm_grid_index), seed,
  rerolls, channel: pattern 2-32 repeats them. Mac render harness
  (scratchpad br/: arch.cpp with a test loop, "@sec:path=value" changes
  parameters during the render): no NaN, peaks < 1, freeze loop exact,
  pattern period exact, sync on the grid. compute() 5.6 kB, ITCM 4
  blocks. Not run on the module yet: check CPU (2 PSRAM reads + 1 write
  per channel) and listen.
- primetime (program 11, 2026-11-17, new, uncommitted): Lexicon Prime
  Time II (Model 95) after its owner's manual (the user's PDF,
  ~/Downloads/f885b7.pdf, scanned: render pages with gs). Tap table 128
  steps a(e^(bn)-1), 0.1..640 ms, prime numbers of 0.1 ms, x memory
  1/2/4 x range. Time base k = 1 + 0.5(sweep(1-depth) + mod depth), taps
  read at d*k. Mac harness (scratchpad pt/: arch8.cpp, INPUT=impulse|loop):
  echo times = table (1X/X2/memory), sweep 0.5-1.5x, repeat loop exact,
  clock pulses exact, no NaN at full feedback + depth. compute() 5.3 kB,
  one 2 MB memory (PSRAM). 2026-11-17: changing delay A/B crackled
  (jumps between steps): two heads per tap crossfaded by a shared 25 ms
  triangle, a head takes the new step only while silent (as mooSpace2);
  the time base k stays continuous; setting change = glide (0.2 s
  smoothing, tape-like pitch). Mac: no sample step larger than the steady
  signal's. CPU on rev2 9 -> 15 % (max 19).
- harmonizer (program 12, 2026-11-17, new, uncommitted): Eventide H949
  after its instruction manual (~/Downloads/H949-Manual.pdf, scanned, no
  theory-of-operation section: the algorithms are inferred). Pointer
  phase advances (1 - r)/W (reverse: (1 + r)/W); alg 2 one head (splices
  at |1-r|/W per s, measured exact), alg 1 two heads W/2 apart with
  triangle crossfades: tones where f*W/2 = k + 1/2 (440 and 1000 Hz at
  W = 25 ms) lose 4.8 dB, shifted up their pitch cancels into a 40 Hz
  warble (comb colouring the manual mentions; down-shifts keep it). Mac
  harness (scratchpad h9/: INPUT=sine with FREQ, impulse, loop; only
  columns 0-1 are real outputs, the rest uninitialized): alg 2 pitch exact
  .25-2.0, delays exact, micro 1.0728/.9272 at the ends, repeat loop exact,
  no NaN, feedback at max + EQ boosts held by the converter clip.
  compute() 7 kB, one 128 kB memory. Not run on the module yet.
- Output setting (clip/soft/tape/fold) on every program's audio outputs
  (nemesis_output.h, `declare audio_outputs`/`declare output`); breaker
  defaults to clip, freqshift shapes 1-6. superosc's oscillator sum scales
  with root+fourth+fifth (it drove its limiter to -8 dB).
- Open: freqshift (rewritten), superosc (drone, MIDI/V-Oct, limiter), CLIP and
  limiter display only simulated, not run on the module; check CPU of both.
- rev2 check 2026-11-17 (firmware with bitrot, primetime, harmonizer,
  PSRAM 144 MHz): all 12 programs switch (heap 444-464 kB after all),
  CPU juno 4, mooSpace 56, mooSpace2 83 (max 92-95), freqshift 16,
  superosc 25, breaker 25, growler 54, pollen 77-80, strike 48, bitrot 8,
  primetime 9, harmonizer 10 %; growler (96k) -> mooSpace2 fine; settings
  and the current program survive a watchdog reset (SD); all 128 juno
  patches load (names = juno.PRE; settings exact, knob-bound values caught
  by the knob when within its pickup window). The transport reported
  53333 BPM with clock source IN 8 and nothing patched (a 2-sample edge
  pair, then stale; programs got it as nm_bpm): CV edges < 1 ms apart are
  ignored and without an active clock bpm = the tempo setting; host test
  tests/transport/run.sh. USB mass storage for the SD card: not wanted
  (the Teensy core has none; MTP needs extra software on macOS).
- Open: MIDI input in the 100 us timer interrupt and the transport PLL are
  only simulated, not yet run on the module (check STATUS, juno notes over
  USB and the jack, breaker on a DAW clock, CPU load of the poll timer).
- Direct TDM, measured 2026-11-06: programs read/write the TDM DMA
  buffers directly (nemesis_tdm.cpp), loopback OUT 1 -> IN 1 281 samples
  (5.85 ms; main 537, the codec adds 25), level identical, "written late" 0
  of 21617 blocks (mooSpace/mooSpace2 at ~60 %). CPU: mooSpace2 58 % (main
  62), breaker 12, growler 14, rest unchanged. The memory plan from
  memoryInfo() (branch memory-plan, not on main) gains nothing (mooSpace v1
  60 % with it, 56 % without): access counts don't predict cost, PSRAM
  reads in order are cheap. Not yet checked: other channels' mapping,
  calibration with the HP, hwtest, rev1.
- Clock source "IN 8" (2026-11-06): clock/gate on audio IN 8, edges found in
  the samples (nemesis_transport.cpp), 1/4..1/16 or 24 ppqn, restarts at the
  downbeat after a pause; tested on the module with the Fireface (out 23 ->
  ES-10 out 1 -> IN 8, scratchpad cvclock.py): 120 and 97 BPM exact. Menus:
  program settings/list close after 2 s (not while editing), system
  settings stay open, positions remembered, scroll bar; fonts on baselines
  8 (6pt) and 29/20 (14pt). Not yet looked at on the display by the user.
- Display fonts (2026-11-09): Michroma 16 px (program names), Michroma 13 px
  oblique (knob popup, splash), Oxanium 10 px (menus, header; list rows on
  baselines 8/19/30); converted with lib/nemesis/src/fonts/ttf2gfx.py (PIL,
  optional slant), OFL licenses next to them. Michroma is unreadable below
  ~10 px (0/8 look like D/B).
- Test scripts: octave "bands" made as x - lowpass are high-passes and hide
  low cuts; use the difference of two low-passes.
- Sample rate per program at runtime (2026-11-11): `declare sample_rate
  "96000"`, system setting auto/48k/96k (not stored); MCLK 49.152 MHz at
  both rates (MFREQ 100, 03h 0xF9), BCLK = MCLK/4 or /2 in RCR2 (the
  receiver makes the clocks), the codec auto-detects 1024x/512x. Powering
  the codec down for MFREQ clicked; the SAI has no BYP bit (bit 23 reads 0). Measured by blocks/s:
  48.0/96.0 kHz, no late blocks. growler (53 %), breaker (29 %), pollen
  (74 %) at 96 kHz, the user found them great (inputs fine);
  mooSpace/mooSpace2 overload at 96k, so they stay at 48k. pollen's loop
  buffer is 2^20 per channel (10.9 s at 96k; 2^19 cut 8/10 s to 5.5 s).
  Outputs fade out/in around switches (5/10 ms), codec DACs muted ~30 ms
  around a rate change. Block
  size 128 is compile time and the max.
- 64 sample blocks, branch block64 (2026-11-09, env rev2_block64,
  -D AUDIO_BLOCK_SAMPLES=64): CPU within ~1 point of 128 for every program
  (mooSpace 57.6, mooSpace2 58.8 max 70 %), no late output blocks; round trip
  128 samples + codec, latency not measured on the loopback yet.
- Block size per program / system setting (branch block-size, 2026-11-09):
  nemesis_tdm.cpp stops the SAI at a frame end, shortens both DMA loops
  (buffers stay 128) and restarts in sync; `declare block_size`, setting
  auto/16/32/64/128. Loopback: 56/88/152/280 samples at 16/32/64/128, level
  identical after 9 switches (slots kept), no late blocks in 64663.
- 32 sample blocks, branch block32 (env rev2_block32): averages again within
  ~1 point (juno 2.4, mooSpace 57.7, mooSpace2 59.7 %), peaks higher
  (mooSpace2 max 74, 83 right after starting), no late output blocks in
  82569. Late-write margin: the transmit DMA leads by a few frames, ~12 % of
  a 32 sample block.
- juno, 6 voices playing (2026-11-09, stereo by note): 65 % CPU, peak 85 %
  at note on (6 moogLadders with per-sample tan/pow); idle 2 %.
- Open: juno stress test with cutoff/resonance/env at max (NaN counter),
  rev1 never tested with the unified code, the 0x44 register mode for DC on
  IN 5/6 untested.
