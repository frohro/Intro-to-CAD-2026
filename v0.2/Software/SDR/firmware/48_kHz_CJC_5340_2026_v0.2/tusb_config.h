/*
 * tusb_config.h — Research: 48 kHz CJC5340 Fixed-Rate UAC1 SDR
 *
 * Changes from Lab 7:
 *   • CFG_TUD_AUDIO_FUNC_1_EP_IN_SZ_MAX  392 → 294
 *     49 samples × 3 bytes/sample × 2 channels (48 kHz with flow-control +1).
 *     TUD_AUDIO_EP_SIZE(false, 48000, 3, 2) = (49+0)*3*2 = 294 bytes.
 *   • CFG_TUD_AUDIO_FUNC_1_EP_IN_SW_BUF_SZ  1536 → 1176
 *     4 × 294 = 1176.
 *
 * No CFG_TUD_AUDIO_FUNC_1_DESC_LEN — UAC2-only requirement.
 * No AUDIO_USE_ASYNC_FEEDBACK — UAC1 uses implicit flow control.
 *
 * CDC macros unchanged from Lab 5/6/7.
 */

#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

//--------------------------------------------------------------------+
// Board / RHPort
//--------------------------------------------------------------------+

// RHPort 0 = RP2040's built-in USB controller
#ifndef BOARD_TUD_RHPORT
#define BOARD_TUD_RHPORT      0
#endif

#ifndef BOARD_TUD_MAX_SPEED
#define BOARD_TUD_MAX_SPEED   OPT_MODE_DEFAULT_SPEED
#endif

//--------------------------------------------------------------------+
// Common TinyUSB configuration
//--------------------------------------------------------------------+

// CFG_TUSB_MCU is injected by the Pico SDK CMake toolchain
#ifndef CFG_TUSB_MCU
#error CFG_TUSB_MCU must be defined
#endif

#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS           OPT_OS_NONE
#endif

#ifndef CFG_TUSB_DEBUG
#define CFG_TUSB_DEBUG        0
#endif

// Enable the device stack (we are a USB device, not a host)
#define CFG_TUD_ENABLED       1

#define CFG_TUD_MAX_SPEED     BOARD_TUD_MAX_SPEED

// On RP2040, USB DMA can reach all of SRAM — no special section needed.
#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif

#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN    __attribute__ ((aligned(4)))
#endif

//--------------------------------------------------------------------+
// Device configuration
//--------------------------------------------------------------------+

#ifndef CFG_TUD_ENDPOINT0_SIZE
#define CFG_TUD_ENDPOINT0_SIZE    64
#endif

//------------- Class drivers -------------//
#define CFG_TUD_CDC              1
#define CFG_TUD_MSC              0
#define CFG_TUD_HID              0
#define CFG_TUD_MIDI             0
#define CFG_TUD_AUDIO            1   // UAC1 stereo S24_3LE capture
#define CFG_TUD_VENDOR           0

//--------------------------------------------------------------------+
// Audio Class Driver Configuration
//--------------------------------------------------------------------+

/* Sample rate for the single-alternate-setting descriptor.
 * Set to 48000 or 96000.  Ensure hardware jumpers match. */
#define CFG_TUD_AUDIO_FUNC_1_SAMPLE_RATE         48000

// Number of Audio Streaming interfaces (one: stereo capture on ITF 3)
#define CFG_TUD_AUDIO_FUNC_1_N_AS_INT            1

// Control request buffer on EP0.
#define CFG_TUD_AUDIO_FUNC_1_CTRL_BUF_SZ        64

// Enable the IN isochronous endpoint
#define CFG_TUD_AUDIO_ENABLE_EP_IN               1

// Maximum IN endpoint packet size.
// For S24_3LE stereo at 48 kHz: (48 + 1) * 3 * 2 = 294 bytes.
#define CFG_TUD_AUDIO_FUNC_1_EP_IN_SZ_MAX       294

// Software FIFO backing the IN endpoint.
// 1176 = 4 intervals * 294
#define CFG_TUD_AUDIO_FUNC_1_EP_IN_SW_BUF_SZ    1176

//--------------------------------------------------------------------+
// CDC FIFO sizes (unchanged from Lab 5/6)
//--------------------------------------------------------------------+

// TX and RX software FIFOs.  64 bytes is ample for short ASCII
// tune commands and acknowledge strings.
#define CFG_TUD_CDC_RX_BUFSIZE   64
#define CFG_TUD_CDC_TX_BUFSIZE   64

// Hardware endpoint buffer (>= bulk packet size = 64 for full-speed)
#define CFG_TUD_CDC_EP_BUFSIZE   64

#ifdef __cplusplus
}
#endif

#endif /* _TUSB_CONFIG_H_ */
