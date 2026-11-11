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

- rev2 test unit: `/dev/cu.usbmodem103226001`, USB MIDI port "Teensy MIDI"
  when built with `-D USB_MIDI_SERIAL` (software/firmware).
- HP 34401A multimeter: `/dev/cu.usbserial-FTEJ7S3Y` (FTDI), RS-232 9600 8N2,
  HP-IB address must not be 31 (talk only). Driver and CLI:
  `~/GitHub/34401a-calibrator` (`hp34401a read -n 5`).
- Fireface UFX outputs 23–30 → ES-10 DC outputs (~11.05 V full scale, only
  handheld-meter accurate; the outputs calibrated against the HP are better).
- The user patches cables and turns knobs on request; say exactly what to
  patch and wait for confirmation.

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
- The transport PLL (`lib/nemesis/src/nemesis_transport_core.h`) is plain C++: test changes on
  the Mac with a host harness before flashing.
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
  3 ms out/in: one block of gate off clicked audibly). Test commands
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
- Firmware programs: juno (0.15: fitted to a recording of A11 (the user's
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
- Output setting (clip/soft/tape/fold) on every program's audio outputs
  (nemesis_output.h, `declare audio_outputs`/`declare output`); breaker
  defaults to clip, freqshift shapes 1-6. superosc's oscillator sum scales
  with root+fourth+fifth (it drove its limiter to -8 dB).
- Open: freqshift (rewritten), superosc (drone, MIDI/V-Oct, limiter), CLIP and
  limiter display only simulated, not run on the module; check CPU of both.
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
