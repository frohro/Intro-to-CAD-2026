#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

#ifndef BOARD_TUD_RHPORT
#define BOARD_TUD_RHPORT      0
#endif

#ifndef BOARD_TUD_MAX_SPEED
#define BOARD_TUD_MAX_SPEED   OPT_MODE_DEFAULT_SPEED
#endif

#ifndef CFG_TUSB_MCU
  #if defined(PICO_RP2350) && PICO_RP2350
    #define CFG_TUSB_MCU      OPT_MCU_RP2350
  #else
    #define CFG_TUSB_MCU      OPT_MCU_RP2040
  #endif
#endif

#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS           OPT_OS_NONE
#endif

#ifndef CFG_TUSB_DEBUG
#define CFG_TUSB_DEBUG        0
#endif

#define CFG_TUD_ENABLED       1
#define CFG_TUD_MAX_SPEED     BOARD_TUD_MAX_SPEED

#ifndef CFG_TUD_ENDPOINT0_SIZE
#define CFG_TUD_ENDPOINT0_SIZE    64
#endif

#define CFG_TUD_CDC              1
#define CFG_TUD_MSC              0
#define CFG_TUD_HID              0
#define CFG_TUD_MIDI             0
#define CFG_TUD_AUDIO            1
#define CFG_TUD_VENDOR           0

#define CFG_TUD_AUDIO_FUNC_1_N_AS_INT            1
#define CFG_TUD_AUDIO_FUNC_1_CTRL_BUF_SZ        64
#define CFG_TUD_AUDIO_ENABLE_EP_IN               1

// 96 kHz S24_3LE stereo: (96+1)*3*2 = 582 bytes
#define CFG_TUD_AUDIO_FUNC_1_EP_IN_SZ_MAX       582
#define CFG_TUD_AUDIO_FUNC_1_EP_IN_SW_BUF_SZ    6984

#define CFG_TUD_CDC_RX_BUFSIZE   128
#define CFG_TUD_CDC_TX_BUFSIZE   128
#define CFG_TUD_CDC_EP_BUFSIZE   64

#ifdef __cplusplus
}
#endif

#endif /* _TUSB_CONFIG_H_ */
