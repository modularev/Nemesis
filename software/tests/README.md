# tests

Hardware test firmware and measurement tools. The firmware projects build
like the examples (`pio run -e rev2 -t upload`).

| folder | what it does |
| --- | --- |
| `hwtest` | shows every input as a bar on the OLED (rev2) and prints it over USB serial, lights the button LEDs while pressed, passes audio 1/2 through and puts a DC level on any output (`DC <0-7> <-1..1>` over serial) |
| `pinscan` | lists which unused Teensy pins change, to find how an unknown control is wired |
| `adcnoise` | measures the pot ADC noise and time per reading for several ADC settings |
| `cvtest` | `cvtest.py`: drives DC levels through a DC coupled audio interface (e.g. an ES-10), measures the CV inputs (`sweep`) and the outputs in loopback (`dac`), needs `hwtest` on the module |

`cvtest/cv_fit.json` holds the CV input measurements of the rev2 test unit
(2026-11-02), `cvtest.py dac` uses it to convert readings to volts.
