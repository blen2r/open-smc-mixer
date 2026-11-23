/* USB descriptors of the mixer probe: one USB MIDI interface (class compliant, no driver). */
#include "tusb.h"

#define USB_VID             0x1209      /* pid.codes */
#define USB_PID             0x0001      /* pid.codes test PID, as the pad's application */

static const tusb_desc_device_t desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = 0x00,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = USB_VID,
    .idProduct          = USB_PID,
    .bcdDevice          = 0x0001,
    .iManufacturer      = 1,
    .iProduct           = 2,
    .iSerialNumber      = 3,
    .bNumConfigurations = 1,
};

const uint8_t *tud_descriptor_device_cb(void)
{
    return (const uint8_t *)&desc_device;
}

#define EPNUM_MIDI_OUT      0x01
#define EPNUM_MIDI_IN       0x81
#define CONFIG_TOTAL_LEN    (TUD_CONFIG_DESC_LEN + TUD_MIDI_DESC_LEN)

static const uint8_t desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, 2, 0, CONFIG_TOTAL_LEN, 0x00, 100),
    TUD_MIDI_DESCRIPTOR(0, 4, EPNUM_MIDI_OUT, EPNUM_MIDI_IN, 64),
};

const uint8_t *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return desc_configuration;
}

static const char *const strings[] = {
    NULL,                   /* 0: language, handled below */
    "open_smc_pad",
    "SMC-Mixer probe",
    "0001",
    "SMC-Mixer probe",
};

static uint16_t desc_str[32];

const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    uint8_t n;

    (void)langid;
    if (index == 0) {
        desc_str[1] = 0x0409;
        n = 1;
    } else {
        const char *s;

        if (index >= sizeof(strings) / sizeof(strings[0])) {
            return NULL;
        }
        s = strings[index];
        for (n = 0; s[n] && n < 31; n++) {
            desc_str[1 + n] = (uint8_t)s[n];
        }
    }
    desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * n + 2));
    return desc_str;
}
