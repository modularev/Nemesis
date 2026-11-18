// USB product name: "Nemesis rev1" / "Nemesis rev2" instead of "Teensy MIDI"
// (the MIDI port's name in macOS and DAWs, tells two units apart).
// Overrides the Teensy core's weak default (usb_names.h).
#include "usb_names.h"

#if NEMESIS_HW_REV == 1
#define PRODUCT_NAME {'N', 'e', 'm', 'e', 's', 'i', 's', ' ', 'r', 'e', 'v', '1'}
#else
#define PRODUCT_NAME {'N', 'e', 'm', 'e', 's', 'i', 's', ' ', 'r', 'e', 'v', '2'}
#endif
#define PRODUCT_NAME_LEN 12

struct usb_string_descriptor_struct usb_string_product_name = {
    2 + PRODUCT_NAME_LEN * 2,
    3,
    PRODUCT_NAME,
};
