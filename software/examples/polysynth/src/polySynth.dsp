import("stdfaust.lib");
declare options "[midi:on]";


freq = nentry("freq",200,40,2000,0.01) : si.polySmooth(gate,0.999,2);
gain = nentry("gain",1,0,1,0.01) : si.polySmooth(gate,0.999,2);
gate = button("gate") : si.smoo; 
cutoff = hslider("cutoff[midi:ctrl 1]",0.5,0.0,1.0,0.001) : si.smoo;
q = nentry("q[midi:ctrl 2]",5,1,50,0.01) : si.smoo;
process = vgroup("synth",os.saw2f4(freq)*gain*gate : ve.oberheimLPF(cutoff,q) <: _,_): co.limiter_1176_R4_stereo;

// import("stdfaust.lib");

// freq = nentry("freq [midi:ctrl 1]",200,40,2000,0.01):si.smoo;
// SR = ma.SR;

// process = os.osci(freq)*0.05 <: _, de.delay(10*SR,  (os.lf_saw(1)+1) * 4.99 * SR );