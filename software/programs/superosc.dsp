//-----------------------------------------------
// Nemesis Porgramming and Uploading Example 
//-----------------------------------------------

import("stdfaust.lib"); // Import all available standard faust libraries 

process =  multi_osc  // 
    with{

        // Define variables
        //------------------------------------
        A0_freq         = 13.75;
        CV_scale        = 100.;

        // Define functions for pitch control
        //------------------------------------
        cv2semi(cv)     = cv * CV_scale; 
        semi2hz(semi)   = A0_freq * pow(2.0, semi/12.0);
        cv2hz           = cv2semi : semi2hz;

        P_init          = 0.;
        P_min           = 0.;
        P_max           = 1.;
        P_step          = 0.001;

        // Set controls
        //--------------
        freq            = hslider("CV_X: Pitch",P_init,0.,1.,0.0001) : si.smooth(0.99) : cv2hz;
        cutoff          = hslider("CV_Y: Cutoff",P_init,0.,1.,0.0001) : _+0.2 : si.smooth(0.99) : cv2hz;
        q               = hslider("CV_Z: Resonance",P_init,0.,1.,0.0001) <: *;

        vol1            = hslider("POT_A: OSC1",P_init,P_min,P_max,P_step) : si.smoo;
        vol2            = hslider("POT_B: OSC2",P_init,P_min,P_max,P_step) : si.smoo;
        vol3            = hslider("POT_C: OSC3",P_init,P_min,P_max,P_step) : si.smoo;
        detune          = hslider("POT_D: Detune",P_init,P_min,P_max,P_step) : _ * 5 : si.smoo;

        osc(freq)       = os.polyblep_saw(freq);

        filt(input) = (input + _  : fi.lowpass(4,cutoff) : aa.tanh1) ~ (_ * -2*q);


        multi_osc = par(i, 4,
        vol1*osc(((i+1))*freq + detune) +
        vol2*osc(((i+1)*1.33) * freq + detune) +
        vol3*osc(((i+1)*1.5) * freq + 2 * detune) :
        _ * 0.5) :> filt,filt;
    };