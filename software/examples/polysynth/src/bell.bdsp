import("stdfaust.lib");

shiftL = hslider("CV_X: Shift", 0., 0, 1, 0.001) <: * : _ * 10000 + 100 ;

process =  _ > 0.01 : par(i, 6, pm.russianBell(i,7000,0.2,0.5));