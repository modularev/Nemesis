import("stdfaust.lib");

voct = hslider("CV_X",0.5,0.,1.,0.0001) : *(100)  : si.smooth(0.993);
freq = nentry("key",19,0,120,0.01) : si.polySmooth(gate,0.999,2) : _ + voct : ba.midikey2hz ;

gate = button("gate") :aa.clip(0.,1.) : si.smooth(0.98); 

filt = hslider("CV_Y",0.8,0.,1.,0.0001) : *(100) + 30 : si.smooth(0.998) : ba.midikey2hz;

q = hslider("CV_Z",0.0,0.,1.,0.0001) <: * : _ * 20 ;
osca = hslider("POT_A",1.0,0.,1.,0.0001) : si.smoo;
oscb = hslider("POT_B",1.0,0.,1.,0.0001) : si.smoo;
oscc = hslider("POT_C",1.0,0.,1.,0.0001) : si.smoo;

reson_filt = ((_*-1) + _  :fi.lowpass(2,filt): aa.tanh1) ~ (*(q,_));

saw_synth = osca * os.saw2f4(freq) + oscb * os.saw2f4(freq/2) + oscc * os.saw2f4(freq * 3)  : reson_filt ;
// fm_synth = os.oscsin(freq+mod)
// with{
//     mod = os.oscsin(20+filt*10000) * q * 1000;
// };

process =  saw_synth <: _,_;