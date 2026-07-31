/*
 * main.c -- Research: 192 kHz UAC1 SDR for Intro-to-CAD-2026 v0.2
 *
 * This version is fixed at 192 kHz using the CJC5340 ADC in I2S master mode.
 *
 * USB bandwidth control:
 *   The ADC still delivers 24-bit samples, but USB sends signed 16-bit stereo
 *   (S16_LE).  audio_task() converts 24 -> 16 with TPDF dither before writing
 *   to TinyUSB.  This reduces the stream from 1152 B/ms (24-bit) to 768 B/ms
 *   nominal at 192 kHz.
 *
 * Dual-core DMA reconfiguration:
 *   Core 0 pushes sentinel 0xFFFFFFFF into the inter-core FIFO.
 *   Core 1 detects the sentinel in its tight loop, stops both DMA channels,
 *   updates their trans_count from g_words_per_buf, restarts channel A,
 *   then acknowledges with 0xFFFFFFFE.
 *   Core 0 spins waiting for the ack before returning.
 *
 * USB audio format:
 *   bSubframeSize=2, bBitResolution=16 (S16_LE).
 *   Each 32-bit DMA word carries the 24-bit sample in bits[31:8]; bits[7:0]
 *   are the CJC5340 zero-pad.  audio_task() packs to 2 bytes per sample:
 *     1) align and sign-extend the 24-bit sample,
 *     2) add TPDF dither,
 *     3) quantize to signed 16-bit,
 *     4) emit little-endian byte pairs.
 *
 * Buffer geometry:
 *   Buffers are sized for 192 kHz: 384 uint32_t words per 1 ms buffer.
 *
 * REQUIRES TinyUSB master (pico-sdk bundles 0.18 which has no UAC1):
 *   git clone https://github.com/hathach/tinyusb ~/tinyusb
 *   export PICO_TINYUSB_PATH=$HOME/tinyusb
 *
 * Pin assignments (Intro-to-CAD-2026 v0.2 board, YD-RP2040 module):
 *   GPIO12  SDA  -- Si5351a I2C data
 *   GPIO13  SCL  -- Si5351a I2C clock
 *   GPIO6   DATA -- CJC5340 SDOUT (PIO in_base)
 *   GPIO7   BCK  -- CJC5340 SCLK  (PIO in_base + 1)
 *   GPIO8   WS   -- CJC5340 LRCK  (PIO in_base + 2)
 *   /RST is wired HIGH on the v0.2 board.
 *   GPIO22  DBG  -- DMA ISR toggle (logic analyser probe, ~500 Hz square wave)
 *
 * Protocol (line-oriented ASCII -- matches quisk_conf_192k.py):
 *
 *   VER
 *       -> VER,SDR C firmware 2.0\r\nOK\r\n
 *
 *   XTAL
 *       -> XTAL,24576000\r\nOK\r\n
 *
 *   MODE
 *       -> MODE,DIRECT\r\nOK\r\n
 *
 *   RATE,<hz>    (192000 only)
 *       -> OK\r\n   (rate applied at next stream start)
 *
 *   FREQ,
 *       -> <last_hz>\r\nOK,<type>,0\r\n
 *
 *   FREQ,<hz>,<N>,<a>,<b>,<c>,<P1>,<P2>,<P3>
 *       -> <hz>\r\nOK,G,0\r\n  or  OK,F,0\r\n
 *
 * Startup sentinel: "SDR ready - waiting for precise integer commands...\r\n"
 * Resent on Ctrl-D (0x04).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "bsp/board_api.h"
#include "tusb.h"

#include "hardware/clocks.h"
#include "hardware/i2c.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/vreg.h"
#include "pico/multicore.h"

#include "i2s_rx.pio.h"
#include "si5351.h"

/* ---- GPIO pin assignments ----------------------------------------- */
#define DATA_PIN      6    /* CJC5340 SDOUT  -> PIO in_base */
#define BCK_PIN       7    /* CJC5340 SCLK   -> PIO in_base + 1 */
#define WS_PIN        8    /* CJC5340 LRCK   -> PIO in_base + 2 */
/* GPIO 9: Reserved / /RST is wired HIGH on v0.2 board */
#define I2C_SDA_PIN   12   /* Si5351a SDA */
#define I2C_SCL_PIN   13   /* Si5351a SCL */
#define MODE_M0_PIN   22   /* Drives CJC5340 M0 for 192 kHz selection */
#define MODE_M1_PIN   26   /* Drives CJC5340 M1 for 192 kHz selection */

/* 100 kHz: required for the MS5351M clone on the 2026 v0.1 board.
 * Increase to 400000U only after verifying with a scope that ACK is seen. */
#define I2C_SPEED_HZ  100000U

/* Board topology — Intro-to-CAD-2026 v0.1 board uses DIRECT (CLK0->I,
 * CLK1->Q) with CLK1_PHOFF = N for exact 90 deg quadrature.
 * A v0.2 board will support either DIRECT or Johnson counter mode;
 * change this to false and update si5351_set_freq_regs() accordingly. */
#define BOARD_DIRECT_MODE true

/* ---- Buffer geometry ---------------------------------------------- */
/*
 * Buffers are sized for 192 kHz.
 * 192 kHz: 192 frames/ms * 2 ch = 384 words = 768 bytes (16-bit packed)
 */
#define MAX_WORDS_PER_BUF   384   /* 192 kHz stereo */

static uint32_t buf_a[MAX_WORDS_PER_BUF];
static uint32_t buf_b[MAX_WORDS_PER_BUF];

/* Runtime rate state (updated by apply_sample_rate(), read by Core 1) */
static volatile uint32_t g_words_per_buf = 384;   /* fixed 192 kHz */
static volatile uint32_t g_sample_rate   = 192000U;
static volatile uint32_t g_pending_rate  = 192000U;
static uint g_pio_offset = 0;

/* ---- DMA channel numbers (claimed at runtime) --------------------- */
static int dma_chan_a;
static int dma_chan_b;

/* ISR invocation counter -- useful for verifying 1 ms cadence */
static volatile uint32_t g_isr_count;

/* Deterministic, fast PRNG for TPDF dithering in the 24->16-bit path. */
static uint32_t s_dither_state = 0x1f2e3d4cu;

static inline uint32_t xorshift32_next(void)
{
    uint32_t x = s_dither_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s_dither_state = x;
    return x;
}

/* ---- CDC line buffer ---------------------------------------------- */
/* LINE_BUF_LEN 80: FREQ command is up to ~60 chars; 80 gives headroom. */
#define LINE_BUF_LEN  80
static char    s_line[LINE_BUF_LEN];
static uint8_t s_line_len = 0;
static bool    s_sdr_ready_sent = false;

/* Current LO state (for bare FREQ, query) */
static uint32_t s_last_hz   = 0;
static char     s_last_type = 'X';   /* 'G', 'F', or 'X' */

/* ---- Helpers ------------------------------------------------------ */
static void cdc_write(const char *s)
{
    tud_cdc_write_str(s);
    tud_cdc_write_flush();
}

/* ---- Forward declarations ----------------------------------------- */
static void cdc_task(void);
static void handle_line(const char *line, uint8_t len);

/* ======================================================================
 * DMA IRQ0 handler -- runs on Core 1
 *
 * Fires every ~1 ms (after each 384-word DMA completion at 192 kHz stereo).
 * The completed channel has already chain-triggered the other channel so
 * audio capture continues without gaps.
 *
 * Responsibilities:
 *   1. Acknowledge and clear the interrupt flag.
 *   2. Reset the completed channel's write_addr and trans_count so it is
 *      ready for the next time its partner chains back to it.
 *   3. Toggle the debug GPIO (~500 Hz square wave on a logic analyser).
 *   4. Post the filled buffer pointer to Core 0 via the inter-core FIFO
 *      (non-blocking, timeout=0: drops if FIFO full rather than stalling).
 * ====================================================================== */
static void __not_in_flash_func(dma_handler)(void)
{
    const uint32_t *filled_buf;
    uint32_t wpb = g_words_per_buf;   /* snapshot -- rate may change between ISRs */

    if (dma_hw->ints0 & (1u << dma_chan_a)) {
        dma_hw->ints0 = 1u << dma_chan_a;
        filled_buf = buf_a;
        dma_channel_set_write_addr(dma_chan_a, buf_a, false);
        dma_channel_set_trans_count(dma_chan_a, wpb, false);
    } else {
        dma_hw->ints0 = 1u << dma_chan_b;
        filled_buf = buf_b;
        dma_channel_set_write_addr(dma_chan_b, buf_b, false);
        dma_channel_set_trans_count(dma_chan_b, wpb, false);
    }

    g_isr_count++;

    /* Non-blocking push -- drop the buffer rather than stall the ISR */
    multicore_fifo_push_timeout_us((uint32_t)(uintptr_t)filled_buf, 0);
}

/* ---- Core 1 entry -- DMA ISR owner -------------------------------- */
/*
 * Rate-change protocol:
 *   Core 0 pushes sentinel 0xFFFFFFFF to signal a rate change.
 *   Core 1 detects it here (outside the ISR, in the polling loop), stops
 *   both DMA channels, re-programs their trans_count from g_words_per_buf
 *   (which Core 0 has already updated), restarts channel A, then pushes
 *   ack 0xFFFFFFFE back to Core 0.
 *
 *   Note: g_words_per_buf is written by Core 0 before pushing the sentinel,
 *   and the FIFO itself acts as the memory barrier that orders the writes.
 */
static void core1_entry(void)
{
    /* Drain stale FIFO values from boot */
    multicore_fifo_drain();

    /* Enable DMA interrupt signalling on both channels */
    dma_channel_set_irq0_enabled(dma_chan_a, true);
    dma_channel_set_irq0_enabled(dma_chan_b, true);

    /* Register handler on Core 1's NVIC so DMA_IRQ_0 fires on Core 1,
     * not on Core 0 where TinyUSB runs.                               */
    irq_set_exclusive_handler(DMA_IRQ_0, dma_handler);
    irq_set_enabled(DMA_IRQ_0, true);

    /* Kick off the ping-pong -- channel A fills first */
    dma_channel_start(dma_chan_a);

    while (true) {
        /* Poll for restart sentinel from Core 0 */
        uint32_t msg;
        if (multicore_fifo_pop_timeout_us(0, &msg)) {
            if (msg == 0xFFFFFFFFu) {
                /* Rate change requested: stop both channels, reconfigure,
                 * restart with the new trans_count already in g_words_per_buf. */
                irq_set_enabled(DMA_IRQ_0, false);
                dma_channel_abort(dma_chan_a);
                dma_channel_abort(dma_chan_b);

                /* Align PIO so the first word captured is guaranteed Left */
                pio_sm_set_enabled(pio0, 0, false);
                pio_sm_clear_fifos(pio0, 0);
                pio_sm_restart(pio0, 0);
                pio_sm_exec(pio0, 0, pio_encode_jmp(g_pio_offset));
                pio_sm_set_enabled(pio0, 0, true);

                uint32_t wpb = g_words_per_buf;
                dma_channel_set_write_addr(dma_chan_a, buf_a, false);
                dma_channel_set_trans_count(dma_chan_a, wpb, false);
                dma_channel_set_write_addr(dma_chan_b, buf_b, false);
                dma_channel_set_trans_count(dma_chan_b, wpb, false);

                irq_set_enabled(DMA_IRQ_0, true);
                dma_channel_start(dma_chan_a);

                /* Acknowledge to Core 0 */
                multicore_fifo_push_blocking(0xFFFFFFFEu);
            }
            /* Any other non-sentinel value from the FIFO is ignored here;
             * normal audio buffer pointers flow from Core 1 to Core 0,
             * not the other direction. */
        }
        tight_loop_contents();
    }
}

/* ======================================================================
 * apply_sample_rate -- Core 0, called from tud_audio_set_itf_cb or init
 *
 * Switches the CJC5340 internal state (software tracking) and reconfigures
 * the DMA ping-pong buffers via a sentinel handshake with Core 1.
 *
 * Note: Hardware rate is set by jumpers on the v0.2 board.
 *
 * Must NOT be called while streaming is active (i.e. call only when the
 * host has selected alt 0, or at startup before Core 1 is launched).
 * ====================================================================== */

static void apply_sample_rate(uint32_t rate)
{
    if (rate != 192000U) return;
    if (rate == g_sample_rate) return;

    /* Update shared state BEFORE signalling Core 1, so Core 1 sees the
     * new g_words_per_buf when it processes the sentinel. */
    g_words_per_buf = 384u;
    g_sample_rate   = rate;

    /* CJC5340 mode is set by hardware jumpers JP_M0/JP_M1 on the v0.2 board.
     * Ensure jumpers are set for 192 kHz operation. */

    /* Signal Core 1 to restart DMA with new trans_count */
    multicore_fifo_push_blocking(0xFFFFFFFFu);

    /* Wait for Core 1 ack */
    uint32_t ack = 0;
    while (!multicore_fifo_pop_timeout_us(100000, &ack) || ack != 0xFFFFFFFEu) {
        tight_loop_contents();
    }
}

/* ======================================================================
 * audio_task -- runs on Core 0 (main loop)
 *
 * Called every iteration of the main loop (~every 1 ms).  Pops one filled
 * DMA buffer pointer from the inter-core FIFO (non-blocking), packs the
 * 32-bit DMA words into 2-byte (S16_LE) USB samples, and writes to
 * TinyUSB's SW FIFO via tud_audio_write().
 *
 * Packing: each DMA word is aligned to signed 24-bit, then TPDF dither is
 * applied before quantizing to signed 16-bit and emitting little-endian bytes.
 *
 * TinyUSB's implicit flow control (CFG_TUD_AUDIO_EP_IN_FLOW_CONTROL=1)
 * varies the IN packet size to absorb clock-rate mismatch without a
 * separate feedback endpoint.
 * ====================================================================== */
static void audio_task(void)
{
    /* 2 bytes per sample: MAX_WORDS_PER_BUF samples * 2 bytes = 768 bytes max */
    static uint8_t packed[MAX_WORDS_PER_BUF * 2];

    uint32_t ptr_val = 0;
    if (!multicore_fifo_pop_timeout_us(0, &ptr_val) || ptr_val == 0) return;

    const uint32_t *src = (const uint32_t *)(uintptr_t)ptr_val;
    uint32_t wpb = g_words_per_buf;

    for (uint32_t i = 0; i < wpb; i++) {
        /* Align to signed 24-bit sample (I2S has a 1-bit delay from WS). */
        int32_t s24 = ((int32_t)(src[i] << 1)) >> 8;

        /* TPDF dither in source-LSB units, then quantize 24->16 with rounding. */
        int32_t tpdf = (int32_t)((xorshift32_next() >> 24) & 0xFF)
                     - (int32_t)((xorshift32_next() >> 24) & 0xFF);
        int32_t s16 = (s24 + tpdf + 128) >> 8;

        if (s16 > 32767) s16 = 32767;
        if (s16 < -32768) s16 = -32768;

        packed[2*i+0] = (uint8_t)(s16 & 0xFF);
        packed[2*i+1] = (uint8_t)((uint32_t)s16 >> 8);
    }

    uint32_t bytes_to_write = wpb * 2u;
    tu_fifo_t *ff = tud_audio_get_ep_in_ff();

    if (ff && tu_fifo_remaining(ff) >= bytes_to_write) {
        tud_audio_write(packed, bytes_to_write);
    } else {
        /* Drop this 1ms frame entirely rather than misaligning the FIFO! */
    }
}

/* ======================================================================
 * UAC1 Audio Callbacks
 *
 * The host sends UAC1 control requests when:
 *  - Opening the audio device (SET_INTERFACE to alt 1)
 *  - Querying / setting sampling frequency (EP control)
 *  - Querying / setting mute (Feature Unit entity control)
 *
 * All SET requests are acknowledged; GET requests return the current
 * (software) values so the host driver is satisfied.
 * ====================================================================== */

/* Track the currently selected alternate setting (0 = idle, 1 = streaming) */
static uint8_t s_audio_alt = 0;

/* Current sampling frequency (fixed 192 kHz) */
static uint32_t s_sample_rate_hz = 192000U;

/* Current mute state per channel: index 0=master, 1=ch1, 2=ch2 */
static uint8_t s_mute[3] = {0, 0, 0};

/* Called when the host selects an alternate setting for an audio interface. */
bool tud_audio_set_itf_cb(uint8_t rhport,
                           tusb_control_request_t const *p_request)
{
    (void)rhport;
    s_audio_alt = (uint8_t)(p_request->wValue & 0xFF);

    /* Apply any pending rate change when the host activates streaming (alt 1).
     * This is the safe window: DMA is running but USB audio is not yet open,
     * so the brief DMA restart doesn't cause audio dropouts. */
    static const uint8_t silence[3072] = {0};
    if (s_audio_alt == 1u) {
        apply_sample_rate(192000U);
        /* Pre-fill ~4 ms of silence at 192 kHz = 3072 bytes (S16_LE stereo).
         * Starting closer to the FIFO midpoint reduces startup underrun risk. */
        tud_audio_write(silence, 3072u);
    }
    return true;
}

/* Called just before the IN endpoint is closed when switching to alt 0. */
bool tud_audio_set_itf_close_ep_cb(uint8_t rhport,
                                    tusb_control_request_t const *p_request)
{
    (void)rhport; (void)p_request;
    return true;
}

/* ---- Endpoint control (sampling frequency) ----------------------- */

/* SET_CUR on the isochronous endpoint -- store new sampling frequency.
 * UAC1 sampling frequency is encoded as a 3-byte little-endian integer.
 * The actual hardware switch happens later in tud_audio_set_itf_cb(). */
bool tud_audio_set_req_ep_cb(uint8_t rhport,
                              tusb_control_request_t const *p_request,
                              uint8_t *pBuff)
{
    (void)rhport;
    uint8_t cs = (uint8_t)(p_request->wValue >> 8);
    if (cs == AUDIO10_EP_CTRL_SAMPLING_FREQ) {
        uint32_t rate = (uint32_t)pBuff[0]
                      | ((uint32_t)pBuff[1] << 8)
                      | ((uint32_t)pBuff[2] << 16);
        if (rate == 192000U) {
            g_pending_rate = rate;
            if (s_audio_alt >= 1u) {
                apply_sample_rate(rate);
            }
        }
        s_sample_rate_hz = rate;   /* keep for GET_CUR response */
    }
    return true;
}

/* GET_CUR on the isochronous endpoint -- return sampling frequency. */
bool tud_audio_get_req_ep_cb(uint8_t rhport,
                              tusb_control_request_t const *p_request)
{
    (void)rhport;
    uint8_t cs = (uint8_t)(p_request->wValue >> 8);
    if (cs == AUDIO10_EP_CTRL_SAMPLING_FREQ) {
        uint8_t freq[3] = {
            (uint8_t)(s_sample_rate_hz),
            (uint8_t)(s_sample_rate_hz >> 8),
            (uint8_t)(s_sample_rate_hz >> 16)
        };
        return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request,
                                                          freq, 3);
    }
    return false;
}

/* ---- Entity control (Feature Unit -- mute) ----------------------- */

/* SET_CUR on Feature Unit -- store mute value for master or a channel.
 * wValue = (CS << 8) | CN  where CS=0x01 (mute), CN=channel number.  */
bool tud_audio_set_req_entity_cb(uint8_t rhport,
                                  tusb_control_request_t const *p_request,
                                  uint8_t *pBuff)
{
    (void)rhport;
    uint8_t cs = (uint8_t)(p_request->wValue >> 8);
    uint8_t cn = (uint8_t)(p_request->wValue & 0xFF);
    if (cs == AUDIO10_FU_CTRL_MUTE && cn < 3u) {
        s_mute[cn] = pBuff[0];
    }
    return true;
}

/* GET_CUR on Feature Unit -- return mute value for the requested channel. */
bool tud_audio_get_req_entity_cb(uint8_t rhport,
                                  tusb_control_request_t const *p_request)
{
    (void)rhport;
    uint8_t cs = (uint8_t)(p_request->wValue >> 8);
    uint8_t cn = (uint8_t)(p_request->wValue & 0xFF);
    if (cs == AUDIO10_FU_CTRL_MUTE) {
        uint8_t val = (cn < 3u) ? s_mute[cn] : 0u;
        return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request,
                                                          &val, 1);
    }
    return false;
}

/* ---- TinyUSB CDC callbacks ---------------------------------------- */
void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts)
{
    (void)itf; (void)rts;
    /* Send "SDR ready" once the host opens the port (DTR asserted).
     * quisk_conf_2026.py waits for this sentinel before sending commands. */
    if (dtr && !s_sdr_ready_sent) {
        cdc_write("SDR ready - waiting for precise integer commands...\r\n");
        s_sdr_ready_sent = true;
    }
    if (!dtr) {
        /* Host closed the port: reset so next open gets the sentinel again. */
        s_sdr_ready_sent = false;
        s_line_len = 0;
    }
}

void tud_cdc_rx_cb(uint8_t itf) { (void)itf; }

/* ---- cdc_task: drain RX FIFO, accumulate a line ------------------- */
static void cdc_task(void)
{
    if (!tud_cdc_connected()) {
        s_line_len = 0;
        return;
    }

    while (tud_cdc_available()) {
        uint8_t ch;
        tud_cdc_read(&ch, 1);

        /* Ctrl-C: clear the current line buffer. */
        if (ch == 0x03) { s_line_len = 0; continue; }

        /* Ctrl-D: re-send the SDR ready sentinel so Quisk can re-sync. */
        if (ch == 0x04) {
            s_line_len = 0;
            cdc_write("SDR ready - waiting for precise integer commands...\r\n");
            continue;
        }

        /* Ignore CR; line terminates on LF */
        if (ch == '\r') continue;

        if (ch == '\n') {
            s_line[s_line_len] = '\0';
            handle_line(s_line, s_line_len);
            s_line_len = 0;
            continue;
        }

        if (s_line_len < LINE_BUF_LEN - 1) {
            s_line[s_line_len++] = (char)ch;
        }
    }
}

/* ---- handle_line: parse one complete line and reply --------------- */
static void handle_line(const char *line, uint8_t len)
{
    if (len == 0) return;

    char reply[96];

    /* VER */
    if (strncmp(line, "VER", 3) == 0) {
        cdc_write("VER,SDR C firmware 2.0 (192kHz)\r\nOK\r\n");
        return;
    }

    /* XTAL */
    if (strncmp(line, "XTAL", 4) == 0) {
        snprintf(reply, sizeof(reply), "XTAL,%lu\r\nOK\r\n",
                 (unsigned long)SI5351_XTAL_FREQ);
        cdc_write(reply);
        return;
    }

    /* MODE */
    if (strncmp(line, "MODE", 4) == 0) {
        cdc_write(BOARD_DIRECT_MODE ? "MODE,DIRECT\r\nOK\r\n"
                                    : "MODE,JOHNSON\r\nOK\r\n");
        return;
    }

    /* RATE (query or set) */
    if (strncmp(line, "RATE", 4) == 0) {
        if (line[4] == ',') {
            uint32_t rate = (uint32_t)strtoul(line + 5, NULL, 10);
            if (rate == 192000U) {
                g_pending_rate = rate;
            }
        } else {
            snprintf(reply, sizeof(reply), "RATE,%lu\r\n", (unsigned long)g_sample_rate);
            cdc_write(reply);
        }
        cdc_write("OK\r\n");
        return;
    }

    /* FREQ, (bare -- return current LO frequency) */
    if (strcmp(line, "FREQ,") == 0) {
        snprintf(reply, sizeof(reply), "%lu\r\nOK,%c,0\r\n",
                 (unsigned long)s_last_hz, s_last_type);
        cdc_write(reply);
        return;
    }

    /* FREQ,<hz>,<N>,<a>,<b>,<c>,<P1>,<P2>,<P3>
     * Quisk supplies all Si5351a register values; firmware just programs them. */
    if (strncmp(line, "FREQ,", 5) == 0) {
        char buf[LINE_BUF_LEN];
        strncpy(buf, line, LINE_BUF_LEN - 1);
        buf[LINE_BUF_LEN - 1] = '\0';

        char *tok = strtok(buf, ",");   /* "FREQ" */
        if (!tok) goto bad_freq;

        tok = strtok(NULL, ","); if (!tok) goto bad_freq;
        uint32_t hz = (uint32_t)strtoul(tok, NULL, 10);

        tok = strtok(NULL, ","); if (!tok) goto bad_freq;
        uint32_t N  = (uint32_t)strtoul(tok, NULL, 10);

        tok = strtok(NULL, ","); if (!tok) goto bad_freq;
        uint32_t a  = (uint32_t)strtoul(tok, NULL, 10);

        tok = strtok(NULL, ","); if (!tok) goto bad_freq;
        uint32_t b  = (uint32_t)strtoul(tok, NULL, 10);

        tok = strtok(NULL, ","); if (!tok) goto bad_freq;
        /* c unused beyond ptype check */

        tok = strtok(NULL, ","); if (!tok) goto bad_freq;
        uint32_t P1 = (uint32_t)strtoul(tok, NULL, 10);

        tok = strtok(NULL, ","); if (!tok) goto bad_freq;
        uint32_t P2 = (uint32_t)strtoul(tok, NULL, 10);

        tok = strtok(NULL, ","); if (!tok) goto bad_freq;
        uint32_t P3 = (uint32_t)strtoul(tok, NULL, 10);

        si5351_set_freq_regs(i2c0, N, P1, P2, P3, BOARD_DIRECT_MODE);

        char ptype = (b == 0) ? 'G' : 'F';
        (void)a;

        s_last_hz   = hz;
        s_last_type = ptype;

        snprintf(reply, sizeof(reply), "%lu\r\nOK,%c,0\r\n",
                 (unsigned long)hz, ptype);
        cdc_write(reply);
        return;

    bad_freq:
        cdc_write("ERR\r\n");
        return;
    }

    /* Unknown command */
    cdc_write("ERR\r\n");
}

/* ---- Main (Core 0) ----------------------------------------------- */
int main(void)
{
    board_init();

    /* 192 kHz quad-speed capture doubles BCK to 12.288 MHz.
     * At the default 125 MHz clk_sys, the PIO slave loop has too little
     * timing margin; 250 MHz restores comfortable headroom. */
    vreg_set_voltage(VREG_VOLTAGE_1_20);
    set_sys_clock_khz(250000, true);

    /* ---- Control GPIOs -------------------------------------------- */
    gpio_init(MODE_M0_PIN);
    gpio_set_dir(MODE_M0_PIN, GPIO_OUT);
    gpio_put(MODE_M0_PIN, 0);

    gpio_init(MODE_M1_PIN);
    gpio_set_dir(MODE_M1_PIN, GPIO_OUT);
    gpio_put(MODE_M1_PIN, 1);

    /* Force variables to fixed 192 kHz operation. */
    g_words_per_buf = 384u;
    g_sample_rate   = 192000U;
    g_pending_rate  = 192000U;

    /* ---- I2C + Si5351a -------------------------------------------- */
    i2c_init(i2c0, I2C_SPEED_HZ);
    gpio_set_function(I2C_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA_PIN);
    gpio_pull_up(I2C_SCL_PIN);
    si5351_init(i2c0);

    /* ---- PIO / I2S state machine ---------------------------------- */
    PIO  pio    = pio0;
    uint sm     = 0;
    g_pio_offset = pio_add_program(pio, &i2s_rx_program);

    pio_sm_config c = i2s_rx_program_get_default_config(g_pio_offset);

    /* CJC5340 outputs DATA (GPIO6), BCK (GPIO7), WS (GPIO8).
     * Set these pins to PIO function and input direction across the range.
     * We enable pull-ups to prevent the I/O error if the CJC5340 is missing. */
    for (int pin = DATA_PIN; pin <= WS_PIN; pin++) {
        pio_gpio_init(pio, pin);
        gpio_pull_up(pin);
    }
    pio_sm_set_consecutive_pindirs(pio, sm, DATA_PIN, 3, false);

    /* in_base = GPIO6; BCK is offset 1, WS is offset 2 */
    sm_config_set_in_pins(&c, DATA_PIN);

    /* Shift LEFT (MSB first), autopush at 32 bits -- one word per channel */
    sm_config_set_in_shift(&c, /*shift_direction=*/false,
                                /*autopush=*/true,
                                /*push_threshold=*/32);

    /* Run PIO at full clk_sys for maximum input timing margin. */
    sm_config_set_clkdiv(&c, 1.0f);

    pio_sm_init(pio, sm, g_pio_offset, &c);
    pio_sm_set_enabled(pio, sm, true);

    /* ---- DMA ping-pong -------------------------------------------- */
    dma_chan_a = dma_claim_unused_channel(true);
    dma_chan_b = dma_claim_unused_channel(true);

    /* Channel A: PIO RX FIFO -> buf_a, chain to B */
    dma_channel_config ca = dma_channel_get_default_config(dma_chan_a);
    channel_config_set_transfer_data_size(&ca, DMA_SIZE_32);
    channel_config_set_read_increment(&ca,  false);   /* PIO FIFO fixed */
    channel_config_set_write_increment(&ca, true);    /* into buffer    */
    channel_config_set_dreq(&ca, pio_get_dreq(pio, sm, false));
    channel_config_set_chain_to(&ca, dma_chan_b);
    dma_channel_configure(dma_chan_a, &ca,
                          buf_a, &pio->rxf[sm], g_words_per_buf, false);

    /* Channel B: PIO RX FIFO -> buf_b, chain to A */
    dma_channel_config cb = dma_channel_get_default_config(dma_chan_b);
    channel_config_set_transfer_data_size(&cb, DMA_SIZE_32);
    channel_config_set_read_increment(&cb,  false);
    channel_config_set_write_increment(&cb, true);
    channel_config_set_dreq(&cb, pio_get_dreq(pio, sm, false));
    channel_config_set_chain_to(&cb, dma_chan_a);
    dma_channel_configure(dma_chan_b, &cb,
                          buf_b, &pio->rxf[sm], g_words_per_buf, false);

    /* ---- Launch Core 1 (registers DMA IRQ on Core 1, starts DMA) -- */
    multicore_launch_core1(core1_entry);

    /* ---- TinyUSB init --------------------------------------------- */
    tusb_rhport_init_t dev_init = {
        .role  = TUSB_ROLE_DEVICE,
        .speed = TUSB_SPEED_AUTO
    };
    tusb_init(BOARD_TUD_RHPORT, &dev_init);

    /* ---- Main loop ------------------------------------------------- */
    while (true) {
        tud_task();       /* USB stack (handles control requests, EP xfers) */
        cdc_task();       /* Quisk LO tune protocol                         */
        audio_task();     /* Pop DMA buffer, write to TinyUSB SW FIFO       */
    }
}
