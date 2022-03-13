import("stdfaust.lib");

voct = hslider("CV_X",0.5,0.,1.,0.0001) : *(100)  : si.smooth(0.993);
freq = nentry("key",19,0,120,0.01) : si.polySmooth(gate,0.999,2) : _ + voct : ba.midikey2hz ;

gate = button("gate") * vca:aa.clip(0.,1.) : si.smoo; 

filt = hslider("CV_Y [midi:ctrl 1]",0.8,0.,1.,0.0001) <: * : si.smooth(0.992);

q = hslider("CV_Z",0.0,0.,1.,0.0001) <: * : _ * 25 ;
vca = hslider("CV_A",1.,0.,1.,0.0001) - 0.005 : si.smooth(0.992);
osca = hslider("POT_A",1.0,0.,1.,0.01) : si.smoo;
oscb = hslider("POT_B",1.0,0.,1.,0.01) : si.smoo;


process = osca * os.saw2f4(freq) + oscb * os.saw2f4(freq / 2) : _ * gate : ve.moogHalfLadder(filt, q) : co.limiter_1176_R4_mono <:_,_; 