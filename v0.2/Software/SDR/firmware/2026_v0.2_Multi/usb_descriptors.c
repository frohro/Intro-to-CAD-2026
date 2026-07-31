/*
 * usb_descriptors.c — Research: 48/96/192 kHz Multi-Rate UAC1 SDR (v0.2)
 *
 * Interface layout:
 * ┌────────────────────────────────────────────────────────────────────┐
 * │  IAD 1 (bFirstInterface=0, bInterfaceCount=2, class=CDC/ACM)     │
 * │  Interface 0 — CDC Control (ACM)                                  │
 * │      EP 0x83 IN  interrupt  8 B   notification                    │
 * │  Interface 1 — CDC Data                                           │
 * │      EP 0x04 OUT bulk  64 B   host→device (tune commands)         │
 * │      EP 0x84 IN  bulk  64 B   device→host (replies)               │
 * │                                                                    │
 * │  IAD 2 (bFirstInterface=2, bInterfaceCount=2, class=Audio)       │
 * │  Interface 2 — Audio Control (UAC1, no EPs)                       │
 * │      Signal chain: IT(ID=1, Line In) → FU(ID=2, mute) → OT(ID=3) │
 * │  Interface 3 alt 0 — Audio Streaming (zero bandwidth)             │
 * │      (no EPs)                                                      │
 * │  Interface 3 alt 1 — Audio Streaming 48 kHz                       │
 * │      EP 0x81 IN  isochronous async 294 B  stereo S24_3LE          │
 * │  Interface 3 alt 2 — Audio Streaming 96 kHz                       │
 * │      EP 0x81 IN  isochronous async 582 B  stereo S24_3LE          │
 * │  Interface 3 alt 3 — Audio Streaming 192 kHz                      │
 * │      EP 0x81 IN  isochronous async 772 B  stereo S16_LE           │
 * └────────────────────────────────────────────────────────────────────┘
 *
 * AUDIO_BLOCK_LEN breakdown (UAC1, three alt settings with 1 freq each):
 *   Manual Audio IAD                                =  8
 *   TUD_AUDIO10_DESC_STD_AC_LEN                     =  9
 *   TUD_AUDIO10_DESC_CS_AC_LEN(1)                   =  9
 *   TUD_AUDIO10_DESC_INPUT_TERM_LEN                 = 12
 *   TUD_AUDIO10_DESC_FEATURE_UNIT_LEN(2)            = 13   (2ch, bCtrlSz=2)
 *   TUD_AUDIO10_DESC_OUTPUT_TERM_LEN                =  9
 *   TUD_AUDIO10_DESC_STD_AS_LEN (alt 0)             =  9
 *   alt 1 block (STD_AS + CS_AS + FORMAT(1) + EP + CS_EP) = 9+7+11+9+7 = 43 (48 kHz S24_3LE)
 *   alt 2 block (same)                              = 43 (96 kHz S24_3LE)
 *   alt 3 block (same)                              = 43 (192 kHz S16_LE)
 *                                               total 198
 */

#include "bsp/board_api.h"   /* board_usb_get_serial() */
#include "tusb.h"

#define _PID_MAP(itf, n)  ( (CFG_TUD_##itf) << (n) )
#define USB_VID   0xCafe
#define USB_PID   (0x4000 | _PID_MAP(CDC,0) | _PID_MAP(MSC,1) | _PID_MAP(HID,2) \
                          | _PID_MAP(MIDI,3) | _PID_MAP(AUDIO,4) | _PID_MAP(VENDOR,5))

/*─────────────────────────────────────────────────────────────────────
 * Device Descriptor
 *─────────────────────────────────────────────────────────────────────*/
static const tusb_desc_device_t desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,           /* USB 2.0 */

    /* Miscellaneous / IAD — required whenever any IAD is present */
    .bDeviceClass       = TUSB_CLASS_MISC,          /* 0xEF */
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,      /* 0x02 */
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,         /* 0x01 */

    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,   /* 64 */

    .idVendor           = USB_VID,          /* 0xCafe */
    .idProduct          = USB_PID,          /* 0x4011 */
    .bcdDevice          = 0x0100,           /* device version 1.0 */

    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,

    .bNumConfigurations = 0x01
};

uint8_t const *tud_descriptor_device_cb(void)
{
    return (uint8_t const *)&desc_device;
}

/*─────────────────────────────────────────────────────────────────────
 * Configuration Descriptor
 *─────────────────────────────────────────────────────────────────────*/

/* Interface numbers — must be consecutive starting at 0. */
enum {
    ITF_NUM_CDC = 0,
    ITF_NUM_CDC_DATA,
    ITF_NUM_AUDIO_CONTROL,    /* = 2 */
    ITF_NUM_AUDIO_STREAMING,  /* = 3  (four alternate settings: 0, 1, 2, 3) */
    ITF_NUM_TOTAL             /* = 4 */
};

#define EPNUM_AUDIO_IN    0x81   /* EP1 IN  — isochronous audio data    */
#define EPNUM_CDC_NOTIF   0x83   /* EP3 IN  — CDC notification          */
#define EPNUM_CDC_OUT     0x04   /* EP4 OUT — CDC bulk data             */
#define EPNUM_CDC_IN      0x84   /* EP4 IN  — CDC bulk data             */

#define AUDIO_CS_AC_TOTALLEN \
    (TUD_AUDIO10_DESC_INPUT_TERM_LEN + \
     TUD_AUDIO10_DESC_FEATURE_UNIT_LEN(2) + \
     TUD_AUDIO10_DESC_OUTPUT_TERM_LEN)

#define AUDIO_BLOCK_LEN  ( \
    8u                                      +  /* Manual Audio IAD              */ \
    TUD_AUDIO10_DESC_STD_AC_LEN             +  /* Std AC Interface              */ \
    TUD_AUDIO10_DESC_CS_AC_LEN(1)           +  /* CS AC Header (1 AS IF)        */ \
    TUD_AUDIO10_DESC_INPUT_TERM_LEN         +  /* Input Terminal  ID=1          */ \
    TUD_AUDIO10_DESC_FEATURE_UNIT_LEN(2)    +  /* Feature Unit    ID=2          */ \
    TUD_AUDIO10_DESC_OUTPUT_TERM_LEN        +  /* Output Terminal ID=3          */ \
    TUD_AUDIO10_DESC_STD_AS_LEN             +  /* AS alt 0 (zero-bw)            */ \
    TUD_AUDIO10_DESC_STD_AS_LEN             +  /* AS alt 1 (48 kHz active)      */ \
    TUD_AUDIO10_DESC_CS_AS_INT_LEN          +  /* CS AS alt 1 Interface         */ \
    TUD_AUDIO10_DESC_TYPE_I_FORMAT_LEN(1)   +  /* Type I Format alt 1 (48 kHz)  */ \
    TUD_AUDIO10_DESC_STD_AS_ISO_EP_LEN      +  /* Standard ISO IN EP alt 1      */ \
    TUD_AUDIO10_DESC_CS_AS_ISO_EP_LEN       +  /* CS AS ISO EP alt 1            */ \
    TUD_AUDIO10_DESC_STD_AS_LEN             +  /* AS alt 2 (96 kHz active)      */ \
    TUD_AUDIO10_DESC_CS_AS_INT_LEN          +  /* CS AS alt 2 Interface         */ \
    TUD_AUDIO10_DESC_TYPE_I_FORMAT_LEN(1)   +  /* Type I Format alt 2 (96 kHz)  */ \
    TUD_AUDIO10_DESC_STD_AS_ISO_EP_LEN      +  /* Standard ISO IN EP alt 2      */ \
    TUD_AUDIO10_DESC_CS_AS_ISO_EP_LEN       +  /* CS AS ISO EP alt 2            */ \
    TUD_AUDIO10_DESC_STD_AS_LEN             +  /* AS alt 3 (192 kHz active)     */ \
    TUD_AUDIO10_DESC_CS_AS_INT_LEN          +  /* CS AS alt 3 Interface         */ \
    TUD_AUDIO10_DESC_TYPE_I_FORMAT_LEN(1)   +  /* Type I Format alt 3 (192 kHz) */ \
    TUD_AUDIO10_DESC_STD_AS_ISO_EP_LEN      +  /* Standard ISO IN EP alt 3      */ \
    TUD_AUDIO10_DESC_CS_AS_ISO_EP_LEN          /* CS AS ISO EP alt 3            */ \
)

#define CONFIG_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + AUDIO_BLOCK_LEN)

#define STRIDX_CDC_IF    4

static const uint8_t desc_configuration[] = {
    /* ── Configuration Descriptor ─────────────────────────────────── */
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),

    /* ── CDC Function (IAD 1) ──────────────────────────────────────── */
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, STRIDX_CDC_IF,
                       EPNUM_CDC_NOTIF, 8,
                       EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),

    /* ── Audio Function (IAD 2) ───────────────────────────────────────── */
    /* Manual Audio Interface Association Descriptor (8 bytes) */
    8, TUSB_DESC_INTERFACE_ASSOCIATION,
    ITF_NUM_AUDIO_CONTROL,            /* bFirstInterface                  */
    2,                                /* bInterfaceCount (AC + AS)        */
    TUSB_CLASS_AUDIO,                 /* bFunctionClass    = 0x01         */
    0x00,                             /* bFunctionSubClass = undefined    */
    0x00,                             /* bFunctionProtocol = V1 (0x00)    */
    0x00,                             /* iFunction                        */

    /* Standard AC Interface Descriptor — 0 endpoints */
    TUD_AUDIO10_DESC_STD_AC(ITF_NUM_AUDIO_CONTROL, 0x00, 0x00),

    /* Class-Specific AC Interface Header */
    TUD_AUDIO10_DESC_CS_AC(0x0100, AUDIO_CS_AC_TOTALLEN, ITF_NUM_AUDIO_STREAMING),

    /* Input Terminal Entity */
    TUD_AUDIO10_DESC_INPUT_TERM(0x01, AUDIO_TERM_TYPE_IN_GENERIC_MIC, 0x03, 2,
                                 (AUDIO10_CHANNEL_CONFIG_LEFT_FRONT |
                                  AUDIO10_CHANNEL_CONFIG_RIGHT_FRONT),
                                 0x00, 0x00),

    /* Feature Unit Entity */
    TUD_AUDIO10_DESC_FEATURE_UNIT(0x02, 0x01, 0x00, AUDIO10_FU_CONTROL_BM_MUTE, 0x0000, 0x0000),

    /* Output Terminal Entity */
    TUD_AUDIO10_DESC_OUTPUT_TERM(0x03, AUDIO_TERM_TYPE_USB_STREAMING, 0x01, 0x02, 0x00),

    /* Standard AS Interface — alt 0: zero bandwidth (no isochronous EP) */
    TUD_AUDIO10_DESC_STD_AS_INT(ITF_NUM_AUDIO_STREAMING, 0x00, 0x00, 0x00),

    /* ── Alt 1: 48 kHz (24-bit PCM) ─────────────────────────────────── */
    /* Standard AS Interface — alt 1 */
    TUD_AUDIO10_DESC_STD_AS_INT(ITF_NUM_AUDIO_STREAMING, 0x01, 0x01, 0x00),

    /* Class-Specific AS Interface — alt 1 */
    TUD_AUDIO10_DESC_CS_AS_INT(0x03, 0x01, AUDIO10_DATA_FORMAT_TYPE_I_PCM),

    /* Type I Format — alt 1: 48 kHz only */
    11,                    /* bLength                  */
    TUSB_DESC_CS_INTERFACE, /* bDescriptorType          */
    0x02,                  /* bDescriptorSubtype: FORMAT_TYPE */
    0x01,                  /* bFormatType: TYPE_I      */
    2,                     /* bNrChannels              */
    3,                     /* bSubframeSize (3 bytes)  */
    24,                    /* bBitResolution (24 bits) */
    1,                     /* bSamFreqType: 1 discrete */
    0x80, 0xBB, 0x00,      /* tSamFreq[0] = 48 000 Hz  */

    /* Standard AS ISO EP — alt 1: 294 B max */
    TUD_AUDIO10_DESC_STD_AS_ISO_EP(EPNUM_AUDIO_IN,
                                    (uint8_t)(TUSB_XFER_ISOCHRONOUS |
                                              TUSB_ISO_EP_ATT_ASYNCHRONOUS),
                                    294, 0x01, 0x00),

    /* Class-Specific AS ISO EP — alt 1 */
    TUD_AUDIO10_DESC_CS_AS_ISO_EP(AUDIO10_CS_AS_ISO_DATA_EP_ATT_SAMPLING_FRQ,
                                   AUDIO10_CS_AS_ISO_DATA_EP_LOCK_DELAY_UNIT_MILLISEC,
                                   0x0001),

    /* ── Alt 2: 96 kHz (24-bit PCM) ─────────────────────────────────── */
    /* Standard AS Interface — alt 2 */
    TUD_AUDIO10_DESC_STD_AS_INT(ITF_NUM_AUDIO_STREAMING, 0x02, 0x01, 0x00),

    /* Class-Specific AS Interface — alt 2 */
    TUD_AUDIO10_DESC_CS_AS_INT(0x03, 0x01, AUDIO10_DATA_FORMAT_TYPE_I_PCM),

    /* Type I Format — alt 2: 96 kHz only */
    11,                    /* bLength                  */
    TUSB_DESC_CS_INTERFACE, /* bDescriptorType          */
    0x02,                  /* bDescriptorSubtype: FORMAT_TYPE */
    0x01,                  /* bFormatType: TYPE_I      */
    2,                     /* bNrChannels              */
    3,                     /* bSubframeSize (3 bytes)  */
    24,                    /* bBitResolution (24 bits) */
    1,                     /* bSamFreqType: 1 discrete */
    0x00, 0x77, 0x01,      /* tSamFreq[0] = 96 000 Hz  */

    /* Standard AS ISO EP — alt 2: 582 B max */
    TUD_AUDIO10_DESC_STD_AS_ISO_EP(EPNUM_AUDIO_IN,
                                    (uint8_t)(TUSB_XFER_ISOCHRONOUS |
                                              TUSB_ISO_EP_ATT_ASYNCHRONOUS),
                                    582, 0x01, 0x00),

    /* Class-Specific AS ISO EP — alt 2 */
    TUD_AUDIO10_DESC_CS_AS_ISO_EP(AUDIO10_CS_AS_ISO_DATA_EP_ATT_SAMPLING_FRQ,
                                   AUDIO10_CS_AS_ISO_DATA_EP_LOCK_DELAY_UNIT_MILLISEC,
                                   0x0001),

    /* ── Alt 3: 192 kHz (16-bit PCM) ────────────────────────────────── */
    /* Standard AS Interface — alt 3 */
    TUD_AUDIO10_DESC_STD_AS_INT(ITF_NUM_AUDIO_STREAMING, 0x03, 0x01, 0x00),

    /* Class-Specific AS Interface — alt 3 */
    TUD_AUDIO10_DESC_CS_AS_INT(0x03, 0x01, AUDIO10_DATA_FORMAT_TYPE_I_PCM),

    /* Type I Format — alt 3: 192 kHz only */
    11,                    /* bLength                  */
    TUSB_DESC_CS_INTERFACE, /* bDescriptorType          */
    0x02,                  /* bDescriptorSubtype: FORMAT_TYPE */
    0x01,                  /* bFormatType: TYPE_I      */
    2,                     /* bNrChannels              */
    2,                     /* bSubframeSize (2 bytes)  */
    16,                    /* bBitResolution (16 bits) */
    1,                     /* bSamFreqType: 1 discrete */
    0x00, 0xEE, 0x02,      /* tSamFreq[0] = 192 000 Hz */

    /* Standard AS ISO EP — alt 3: 772 B max */
    TUD_AUDIO10_DESC_STD_AS_ISO_EP(EPNUM_AUDIO_IN,
                                    (uint8_t)(TUSB_XFER_ISOCHRONOUS |
                                              TUSB_ISO_EP_ATT_ASYNCHRONOUS),
                                    772, 0x01, 0x00),

    /* Class-Specific AS ISO EP — alt 3 */
    TUD_AUDIO10_DESC_CS_AS_ISO_EP(AUDIO10_CS_AS_ISO_DATA_EP_ATT_SAMPLING_FRQ,
                                   AUDIO10_CS_AS_ISO_DATA_EP_LOCK_DELAY_UNIT_MILLISEC,
                                   0x0001)
};

uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return desc_configuration;
}

/*─────────────────────────────────────────────────────────────────────
 * String Descriptors
 *─────────────────────────────────────────────────────────────────────*/
static const char *const string_desc_table[] = {
    [0] = (const char[]){ 0x09, 0x04 },  /* LangID 0x0409: US English         */
    [1] = "WWU",                         /* iManufacturer                     */
    [2] = "WWU SDR",                     /* iProduct                          */
    [3] = "123456",                      /* iSerialNumber (overridden at runtime) */
    [4] = "WWU SDR Control"              /* STRIDX_CDC_IF                     */
};

static uint16_t desc_string[32];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;
    uint8_t chr_count = 0;

    if (index == 0) {
        memcpy(&desc_string[1], string_desc_table[0], 2);
        chr_count = 1;
    } else {
        const char *str = NULL;
        char serial_buf[32];

        if (index == 3) {
            /* Read unique board ID for serial string */
            board_usb_get_serial(serial_buf, sizeof(serial_buf));
            str = serial_buf;
        } else if (index < sizeof(string_desc_table) / sizeof(string_desc_table[0])) {
            str = string_desc_table[index];
        }

        if (!str) return NULL;

        chr_count = (uint8_t)strlen(str);
        if (chr_count > 31) chr_count = 31;

        for (uint8_t i = 0; i < chr_count; i++) {
            desc_string[1 + i] = (uint16_t)str[i];
        }
    }

    desc_string[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
    return desc_string;
}
