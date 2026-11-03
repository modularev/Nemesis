# programs

All FAUST programs of the module. `../firmware` builds them into one
firmware for rev2 (encoder to choose), `../single` builds one of them alone
(rev1, or to try a program). Parameters bind to the hardware by their label
metadata (`[hw:..]`, `[rev1:..]`, `[rev2:..]`, `[cv:..]`, see
`../lib/nemesis/src/nemesis_program.cpp`); on rev2 the parameters without a
knob are settings (turn the encoder). The comment at the top of each `.dsp`
has the details.

| program | what | rev2 controls | rev1 controls |
| --- | --- | --- | --- |
| `juno.dsp` | 6 voice MIDI polysynth with chorus, stereo out 1/2 | POT A–D cutoff, resonance, env, release; X/Y/Z LFO rate, PWM, chorus; envelope and mixer as settings and MIDI CC | knobs A–D, X/Y/Z |
| `mooSpace.dsp` | stereo reverb, in/out 1/2 | X/Y/Z smear, decay, drag; POT A–D filter, modulate, colour, mix (+ CV A–D); lag, push as settings | CV X–Z, A–F (smear decay drag filter modulate colour lag push mix) |
| `freqshift.dsp` | stereo frequency shifter | X/Z shift L/R, Y shift scale, POT A mix | CV X/Y/Z, A |
| `superosc.dsp` | three oscillators with filter | X pitch, Y cutoff, Z resonance, POT A–D osc 1–3 level, detune | CV X/Y/Z, A–D |
| `breaker.dsp` | beat slice repeat/reverse, crush, comb, drive; MIDI clock or tempo | X slice, Y/Z repeat/reverse chance, CV A/B gates, POT A–D crush, drive, comb, feedback; tempo, mix as settings | written for rev2 |

`sketches/` holds unfinished programs (bell, mono_voice, polySynth); they
are not built. Copy one up here and add it to a firmware to try it.
