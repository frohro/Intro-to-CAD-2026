/*
 * usb_descriptors.c — Research: 192 kHz UAC1 SDR
 *
 * Changes from Lab 7:
 *
 *   bSubframeSize: 2        (S16_LE)
 *   bBitResolution: 16
 *   bSamFreqType: 1         (single fixed rate)
 *   Alt 1: wMaxPacketSize = 772, 192 kHz only
 *   AUDIO_BLOCK_LEN: 112
 *
 * Interface layout (same as Lab 7):
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
 * │  Interface 3 alt 1 — Audio Streaming 192 kHz                      │
 * │      EP 0x81 IN  isochronous async 772 B  stereo S16_LE           │
 * └────────────────────────────────────────────────────────────────────┘
 *
 * AUDIO_BLOCK_LEN breakdown (UAC1, one alt setting with 1 freq):
 *   Manual Audio IAD                                =  8
 *   TUD_AUDIO10_DESC_STD_AC_LEN                     =  9
 *   TUD_AUDIO10_DESC_CS_AC_LEN(1)                   =  9
 *   TUD_AUDIO10_DESC_INPUT_TERM_LEN                 = 12
 *   TUD_AUDIO10_DESC_FEATURE_UNIT_LEN(2)            = 13   (2ch, bCtrlSz=2)
 *   TUD_AUDIO10_DESC_OUTPUT_TERM_LEN                =  9
 *   TUD_AUDIO10_DESC_STD_AS_LEN (alt 0)             =  9
 *   alt 1 block (STD_AS + CS_AS + FORMAT(1) + EP + CS_EP) = 9+7+11+9+7 = 43
 *                                               total 112
 */

#include "bsp/board_api.h"   /* board_usb_get_serial() */
#include "tusb.h"

/* ── Product ID ────────────────────────────────────────────────────
 * The bitmap encodes which USB classes are active so that Windows does
 * not reuse a cached driver from a previous, different configuration.
 *
 *   Bit 0 — CDC    Bit 1 — MSC    Bit 2 — HID
 *   Bit 3 — MIDI   Bit 4 — AUDIO  Bit 5 — VENDOR
 *
 * Lab 5 (CDC only):     PID = 0x4000 | (1<<0)          = 0x4001
 * Lab 6 (CDC + AUDIO):  PID = 0x4000 | (1<<0) | (1<<4) = 0x4011
 * The change forces the host OS to re-enumerate — intentional.
 */
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
    .idProduct          = USB_PID,          /* 0x4011 for Lab 6 */
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
    ITF_NUM_AUDIO_STREAMING,  /* = 3  (two alternate settings: 0 and 1) */
    ITF_NUM_TOTAL             /* = 4 */
};

/* ── Endpoint assignments ───────────────────────────────────────────
 *
 * Audio MUST use EP1 for its isochronous endpoint (lowest latency slot).
 * UAC1 async capture uses a single IN endpoint — no feedback EP needed.
 *
 * Audio:
 *   0x81 — EP1 IN  isochronous async     772 B   stereo 16-bit PCM data
 *
 * CDC (shifted from Lab 5):
 *   0x83 — EP3 IN  interrupt              8 B    CDC notification (was 0x81)
 *   0x04 — EP4 OUT bulk                  64 B   host→device commands (was 0x02)
 *   0x84 — EP4 IN  bulk                  64 B   device→host replies  (was 0x82)
 */
#define EPNUM_AUDIO_IN    0x81   /* EP1 IN  — isochronous audio data    */
#define EPNUM_CDC_NOTIF   0x83   /* EP3 IN  — CDC notification          */
#define EPNUM_CDC_OUT     0x04   /* EP4 OUT — CDC bulk data             */
#define EPNUM_CDC_IN      0x84   /* EP4 IN  — CDC bulk data             */

/* ── UAC1 Audio Control class-specific wTotalLength ────────────────
 * _totallen parameter to TUD_AUDIO10_DESC_CS_AC() = sum of the CS
 * entity descriptors that follow (the macro adds the CS AC header
 * length itself, so wire wTotalLength = _totallen + CS_AC_LEN(1) = 43).
 *
 *   Input Terminal  (12) + Feature Unit (13) + Output Terminal (9) = 34
 */
#define AUDIO_CS_AC_TOTALLEN \
    (TUD_AUDIO10_DESC_INPUT_TERM_LEN + \
     TUD_AUDIO10_DESC_FEATURE_UNIT_LEN(2) + \
     TUD_AUDIO10_DESC_OUTPUT_TERM_LEN)

/* ── Total configuration descriptor length ─────────────────────────
 *   TUD_CONFIG_DESC_LEN  =   9
 *   TUD_CDC_DESC_LEN     =  66
 *   AUDIO_BLOCK_LEN      = 112  (see file header for byte-by-byte breakdown)
 */
#define AUDIO_BLOCK_LEN  ( \
    8u                                      +  /* Manual Audio IAD              */ \
    TUD_AUDIO10_DESC_STD_AC_LEN             +  /* Std AC Interface              */ \
    TUD_AUDIO10_DESC_CS_AC_LEN(1)           +  /* CS AC Header (1 AS IF)        */ \
    TUD_AUDIO10_DESC_INPUT_TERM_LEN         +  /* Input Terminal  ID=1          */ \
    TUD_AUDIO10_DESC_FEATURE_UNIT_LEN(2)    +  /* Feature Unit    ID=2          */ \
    TUD_AUDIO10_DESC_OUTPUT_TERM_LEN        +  /* Output Terminal ID=3          */ \
    TUD_AUDIO10_DESC_STD_AS_LEN             +  /* AS alt 0 (zero-bw)            */ \
    TUD_AUDIO10_DESC_STD_AS_LEN             +  /* AS alt 1 (Active 48k)         */ \
    TUD_AUDIO10_DESC_CS_AS_INT_LEN          +  /* CS AS Interface               */ \
    TUD_AUDIO10_DESC_TYPE_I_FORMAT_LEN(1)   +  /* Type I Format                 */ \
    TUD_AUDIO10_DESC_STD_AS_ISO_EP_LEN      +  /* Standard ISO IN EP            */ \
    TUD_AUDIO10_DESC_CS_AS_ISO_EP_LEN          /* CS AS ISO EP                  */ \
)

#define CONFIG_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + AUDIO_BLOCK_LEN)

/* String index for the CDC interface name */
#define STRIDX_CDC_IF    4

static const uint8_t desc_configuration[] = {
    /* ── Configuration Descriptor ─────────────────────────────────── */
    /* config_num=1, interface_count, string_idx=0, total_len,
     * attributes (bus-powered, no remote wakeup), 100 mA           */
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),

    /* ── CDC Function (IAD 1) ──────────────────────────────────────── */
    /* itf_num, string_idx, notif_ep+size, data_epout, data_epin, ep_size */
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, STRIDX_CDC_IF,
                       EPNUM_CDC_NOTIF, 8,
                       EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),

    /* ── Audio Function (IAD 2) ─────────────────────────────────────────
     *
     * No TUD_AUDIO10_DESC_IAD macro exists; build the 8-byte IAD manually.
     * bFunctionProtocol = 0x00 (UAC1 uses protocol code "undefined").
     * ──────────────────────────────────────────────────────────────────── */

    /* Manual Audio Interface Association Descriptor (8 bytes) */
    8, TUSB_DESC_INTERFACE_ASSOCIATION,
    ITF_NUM_AUDIO_CONTROL,            /* bFirstInterface                  */
    2,                                /* bInterfaceCount (AC + AS)        */
    TUSB_CLASS_AUDIO,                 /* bFunctionClass    = 0x01         */
    0x00,                             /* bFunctionSubClass = undefined    */
    0x00,                             /* bFunctionProtocol = V1 (0x00)    */
    0x00,                             /* iFunction                        */

    /* Standard AC Interface Descriptor (4.3.1) — 0 endpoints */
    TUD_AUDIO10_DESC_STD_AC(/*_itfnum*/ ITF_NUM_AUDIO_CONTROL,
                             /*_nEPs*/   0x00,
                             /*_stridx*/ 0x00),

    /* Class-Specific AC Interface Header (4.3.2)
     * bcdADC=0x0100 (UAC1).  _totallen = IT(12)+FU(13)+OT(9)=34.
     * Macro adds CS_AC_LEN(1)=9 → wire wTotalLength = 43. */
    TUD_AUDIO10_DESC_CS_AC(/*_bcdADC*/  0x0100,
                            /*_totallen*/ AUDIO_CS_AC_TOTALLEN,
                            /*_itf*/     ITF_NUM_AUDIO_STREAMING),

    /* Input Terminal Entity (4.3.2.1)
     * ID=1, type=Microphone/Line-In, assocTerm=3 (paired Output Terminal),
     * 2 channels (L+R), no channel name strings. */
    TUD_AUDIO10_DESC_INPUT_TERM(/*_termid*/          0x01,
                                 /*_termtype*/        AUDIO_TERM_TYPE_IN_GENERIC_MIC,
                                 /*_assocTerm*/       0x03,
                                 /*_nchannels*/       2,
                                 /*_channelcfg*/      (AUDIO10_CHANNEL_CONFIG_LEFT_FRONT |
                                                       AUDIO10_CHANNEL_CONFIG_RIGHT_FRONT),
                                 /*_idxchannelnames*/ 0x00,
                                 /*_stridx*/          0x00),

    /* Feature Unit Entity (4.3.2.5)
     * ID=2, source=IT(1).  bControlSize=2 (hardcoded in macro).
     * 3 bmaControls: master(mute only), ch1(none), ch2(none).
     * TU_ARGS_NUM(...)-1 = 3-1 = 2 channels → LEN = 7+(2+1)*2 = 13 bytes. */
    TUD_AUDIO10_DESC_FEATURE_UNIT(/*_unitid*/ 0x02,
                                   /*_srcid*/  0x01,
                                   /*_stridx*/ 0x00,
                                   /*master*/  AUDIO10_FU_CONTROL_BM_MUTE,
                                   /*ch1*/     0x0000,
                                   /*ch2*/     0x0000),

    /* Output Terminal Entity (4.3.2.2)
     * ID=3, type=USB Streaming, assocTerm=1 (paired Input Terminal),
     * source=FU(2). */
    TUD_AUDIO10_DESC_OUTPUT_TERM(/*_termid*/    0x03,
                                  /*_termtype*/  AUDIO_TERM_TYPE_USB_STREAMING,
                                  /*_assocTerm*/ 0x01,
                                  /*_srcid*/     0x02,
                                  /*_stridx*/    0x00),

    /* Standard AS Interface — alt 0: zero bandwidth (no isochronous EP) */
    TUD_AUDIO10_DESC_STD_AS_INT(/*_itfnum*/ ITF_NUM_AUDIO_STREAMING,
                                 /*_altset*/ 0x00,
                                 /*_nEPs*/   0x00,
                                 /*_stridx*/ 0x00),

    /* Standard AS Interface — alt 1: Active 192 kHz (1 isochronous EP) */
    TUD_AUDIO10_DESC_STD_AS_INT(/*_itfnum*/ ITF_NUM_AUDIO_STREAMING,
                                 /*_altset*/ 0x01,
                                 /*_nEPs*/   0x01,
                                 /*_stridx*/ 0x00),

    /* Class-Specific AS Interface */
    TUD_AUDIO10_DESC_CS_AS_INT(/*_termid*/      0x03,
                                /*_delay*/      0x01,
                                /*_formattype*/ AUDIO10_DATA_FORMAT_TYPE_I_PCM),

    /* Type I Format (192 kHz)
     * Wire layout (11 bytes = 8 fixed + 3 * 1 freq):
     *   bSamFreqType = 1, tSamFreq[0] = 192000 = 0x02EE00 */
    11,                    /* bLength                  */
    TUSB_DESC_CS_INTERFACE, /* bDescriptorType          */
    0x02,                  /* bDescriptorSubtype: FORMAT_TYPE */
    0x01,                  /* bFormatType: TYPE_I      */
    2,                     /* bNrChannels              */
    2,                     /* bSubframeSize            */
    16,                    /* bBitResolution           */
    1,                     /* bSamFreqType: 1 discrete */
    0x00, 0xEE, 0x02,      /* tSamFreq[0] = 192 000 Hz */

    /* Standard AS ISO EP: 192 kHz
     * 772 bytes for 192 kHz (192+1 frames * 2 bytes * 2 ch = 772) */
    TUD_AUDIO10_DESC_STD_AS_ISO_EP(/*_ep*/       EPNUM_AUDIO_IN,
                                    /*_attr*/     (uint8_t)(TUSB_XFER_ISOCHRONOUS |
                                                            TUSB_ISO_EP_ATT_ASYNCHRONOUS),
                                    /*_maxEPsize*/ 772,
                                    /*_interval*/  0x01,
                                    /*_sync_ep*/   0x00),

    /* Class-Specific AS ISO EP */
    TUD_AUDIO10_DESC_CS_AS_ISO_EP(/*_attr*/          AUDIO10_CS_AS_ISO_DATA_EP_ATT_SAMPLING_FRQ,
                                   /*_lockdelayunits*/ AUDIO10_CS_AS_ISO_DATA_EP_LOCK_DELAY_UNIT_MILLISEC,
                                   /*_lockdelay*/      0x0001),
};

uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;    /* single configuration */
    return desc_configuration;
}

/*─────────────────────────────────────────────────────────────────────
 * String Descriptors
 *─────────────────────────────────────────────────────────────────────
 * Index   Content
 *   0     Language ID array — 0x0409 = English (United States)
 *   1     Manufacturer
 *   2     Product
 *   3     Serial — filled from RP2040 unique ID via board_usb_get_serial()
 *   4     CDC interface name ("LO Control")
 */
enum {
    STRID_LANGID = 0,
    STRID_MANUFACTURER,
    STRID_PRODUCT,
    STRID_SERIAL,
    STRID_CDC_IF,
};

static const char *const string_desc_arr[] = {
    [STRID_LANGID]       = (const char[]){ 0x09, 0x04 },  /* English */
    [STRID_MANUFACTURER] = "WWU CPTR 480",
    [STRID_PRODUCT]      = "WWU SDR",
    [STRID_SERIAL]       = NULL,            /* filled by board_usb_get_serial() */
    [STRID_CDC_IF]       = "LO Control",
};

/* Working buffer for UTF-16 conversion (1 header word + up to 32 chars) */
static uint16_t _desc_str[33];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;

    size_t chr_count;

    if (index == STRID_LANGID) {
        /* Language ID — copy the 2-byte code directly */
        memcpy(&_desc_str[1], string_desc_arr[STRID_LANGID], 2);
        chr_count = 1;

    } else if (index == STRID_SERIAL) {
        /* RP2040 unique 64-bit ID formatted as a hex string */
        chr_count = board_usb_get_serial(_desc_str + 1, 32);

    } else {
        /* Validate index */
        if (index >= sizeof(string_desc_arr) / sizeof(string_desc_arr[0]))
            return NULL;

        const char *str = string_desc_arr[index];
        if (!str) return NULL;

        /* Convert ASCII → UTF-16LE, cap at 32 characters */
        chr_count = strlen(str);
        if (chr_count > 32) chr_count = 32;
        for (size_t i = 0; i < chr_count; i++)
            _desc_str[1 + i] = (uint16_t)str[i];
    }

    /* Descriptor header: length in bytes (2 per char + 2 for header),
     * descriptor type = STRING                                         */
    _desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));

    return _desc_str;
}
