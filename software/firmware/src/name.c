// USB product name: "Nemesis" instead of "Teensy MIDI" (the MIDI port's name
// in macOS and DAWs). Overrides the Teensy core's weak default (usb_names.h).
#include "usb_names.h"

#define PRODUCT_NAME {'N', 'e', 'm', 'e', 's', 'i', 's'}
#define PRODUCT_NAME_LEN 7

struct usb_string_descriptor_struct usb_string_product_name = {
    2 + PRODUCT_NAME_LEN * 2,
    3,
    PRODUCT_NAME,
};
