# mooSpace

Variable space stereo reverb with input drive, filter and modulation in the
tank. Build with `pio run -e rev2 -t upload` (or `-e rev1`).

| parameter | rev1 | rev2 |
| --- | --- | --- |
| smear, decay, drag | CV_X, CV_Y, CV_Z | CV_X, CV_Y, CV_Z |
| Filter, modulate | CV_A, CV_B | CV_A, CV_B |
| colour, lag, push, mix | CV_C, CV_D, CV_E, CV_F | POT_A, POT_B, POT_C, POT_D |

Audio in and out are channels 1 and 2.
