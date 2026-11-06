# programs

All FAUST programs of the module. `../firmware` builds them into one
firmware for rev2 (encoder to choose), `../single` builds one of them alone
(rev1, or to try a program). Parameters bind to the hardware by their label
metadata (`[hw:..]`, `[rev1:..]`, `[rev2:..]`, `[cv:..]`, see
`../lib/nemesis/src/nemesis_program.cpp`); on rev2 the parameters without a
knob are settings (turn the encoder). Programs that follow the tempo use the
system transport with `import("nemesis.lib");` (`../faust/nemesis.lib`).
The comment at the top of each `.dsp` has the details.

| program | what | rev2 controls | rev1 controls |
| --- | --- | --- | --- |
| `juno.dsp` | 6 voice MIDI polysynth with chorus, stereo out 1/2 | POT A–D cutoff, resonance, env, release; X/Y/Z LFO rate, PWM, chorus; envelope and mixer as settings and MIDI CC | knobs A–D, X/Y/Z |
| `mooSpace.dsp` | stereo reverb, in/out 1/2 | X/Y/Z smear, decay, drag; POT A–D filter, modulate, colour, mix (+ CV A–D); lag, push as settings | CV X–Z, A–F (smear decay drag filter modulate colour lag push mix) |
| `mooSpace2.dsp` | mooSpace v1's algorithm with wider controls: drag to 2x, low smear lowers the diffusion (distinct echoes), deeper modulation, darker colour (also with a short decay), filter make-up gain, limiter; motion setting: tank delays glide (pitch) or crossfade (no pitch) | as mooSpace (X smear, Y decay, Z drag; POT A–D filter, modulate, colour, mix); lag, push, motion, crossfade as settings | as mooSpace |
| `freqshift.dsp` | stereo frequency shifter with feedback delay; out 1/2 main, 3/4 up, 5/6 down, 7/8 quadrature oscillator | X shift, Y range (10 Hz–4 kHz), Z stereo spread; POT A–D mix, delay, feedback, sideband (+ CV A–D); damp as setting | CV X/Y/Z, A–D |
| `superosc.dsp` | stereo drone: root/fourth/fifth as detuned saw partials, ladder filter, limiter; MIDI note + V/Oct; out 3/4 pitch CV and gate (MIDI to CV) | X tune, Y cutoff, Z resonance, CV A V/Oct, POT A–D root, fourth, fifth, detune; octave, drift (CC 1), gated, swell, level as settings | CV X/Y/Z, A–D, V/Oct on CV E |
| `growler.dsp` | mono FM bass: modulator with ratio and self feedback into a feedback carrier, wavefolder, ladder filter, decay envelope on index and cutoff, wobble LFO synced to the transport, clean sub; MIDI or V/Oct + gate; out 3/4 wobble and envelope CV | X growl (FM index), Y cutoff, Z wobble rate (off..1/32), CV A V/Oct, CV B gate, POT A–D ratio, feedback, drive, decay; wobble depth (CC 1), env filter, resonance, sub, octave, glide, attack, release, velocity, level as settings | CV X/Y/Z, A–D, V/Oct on CV E, gate on CV F |
| `breaker.dsp` | beat slice repeat/reverse, crush, comb, drive on the transport (MIDI clock or tempo); outs 3–8 drum triggers (kick, snare, closed/open hh, perc 1/2) from a 4×4 morphing pattern map | X slice, Y rev-rep chance (centre off), Z bipolar comb, CV A/B gates, POT A/B crush, drive, POT C/D (+ CV C/D) pattern x/y; mix, density, swing, trig length as settings | written for rev2 |

`sketches/` holds unfinished programs (bell, mono_voice, polySynth); they
are not built. Copy one up here and add it to a firmware to try it.
