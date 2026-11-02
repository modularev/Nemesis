// hardware test: audio pass-through on channels 1 and 2 plus an adjustable
// DC level on all 8 outputs (dc0 .. dc7, set over USB serial: "DC <0-7> <-1..1>")
import("stdfaust.lib");

process = si.bus(2), par(i, 6, 0) : par(i, 8, _ + hslider("dc%i", 0, -1, 1, 0.0001));
