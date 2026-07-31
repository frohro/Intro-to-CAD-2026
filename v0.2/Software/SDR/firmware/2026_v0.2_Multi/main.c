/*
 * main.c -- Research: Multi-ADC dynamic rate UAC1 SDR for Intro-to-CAD-2026 v0.2
 *
 * This version supports both the PCM1808 and CJC5430 ADCs in I2S master mode,
 * with runtime switching of ADCs and sample rates (48 kHz, 96 kHz, 192 kHz).
 *
 * USB bandwidth control:
 *   At 48/96 kHz, true 24-bit samples are sent (S24_3LE).
 *   At 192 kHz, samples are converted 24 -> 16 with TPDF dither (S16_LE)
 *   to fit within the USB Full-Speed bandwidth.
 *
 * Dual-core DMA reconfiguration:
 *   Core 0 pushes sentinel 0xFFFFFFFF into the inter-core FIFO.
 *   Core 1 detects the sentinel in its tight loop, stops both DMA channels,
 *   updates their trans_count from g_words_per_buf, restarts channel A,
 *   then acknowledges with 0xFFFFFFFE.
 *   Core 0 spins waiting for the ack before returning.
 *
 * Pin assignments (Intro-to-CAD-2026 v0.2 board, YD-RP2040 module):
 *   GPIO12  SDA  -- Si5351a I2C data
 *   GPIO13  SCL  -- Si5351a I2C clock
 *   GPIO6   DATA -- CJC5430 DATA (PIO in_base when active)
 *   GPIO7   BCK  -- CJC5430 BCK  (PIO in_base + 1 when active)
 *   GPIO8   WS   -- CJC5430 WS   (PIO in_base + 2 when active)
 *   GPIO9   DATA -- PCM1808 DATA (PIO in_base when active)
 *   GPIO10  BCK  -- PCM1808 BCK  (PIO in_base + 1 when active)
 *   GPIO11  WS   -- PCM1808 WS   (PIO in_base + 2 when active)
 *   GPIO22  M0   -- Mode Select 0
 *   GPIO26  M1   -- Mode Select 1
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
#define I2C_SDA_PIN   12   /* Si5351a SDA */
#define I2C_SCL_PIN   13   /* Si5351a SCL */
#define MODE_M0_PIN   22   /* Mode Select 0 */
#define MODE_M1_PIN   26   /* Mode Select 1 */

#define I2C_SPEED_HZ  100000U

/* Board topology — v0.2 board uses Johnson counter mode */
#define BOARD_DIRECT_MODE true

/* ---- Buffer geometry ---------------------------------------------- */
#define MAX_WORDS_PER_BUF   384   /* 192 kHz stereo */

static uint32_t buf_a[MAX_WORDS_PER_BUF];
static uint32_t buf_b[MAX_WORDS_PER_BUF];

typedef enum {
    ADC_PCM1808 = 0,
    ADC_CJC5430 = 1
} adc_type_t;

/* Runtime state */
static volatile adc_type_t g_adc_type = ADC_PCM1808;
static volatile uint32_t g_words_per_buf = 96;
static volatile uint32_t g_sample_rate   = 48000U;
static volatile uint32_t g_pending_rate  = 48000U;
static uint g_pio_offset = 0;

/* ---- DMA channel numbers (claimed at runtime) --------------------- */
static int dma_chan_a;
static int dma_chan_b;

/* ISR invocation counter */
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
#define LINE_BUF_LEN  80
static char    s_line[LINE_BUF_LEN];
static uint8_t s_line_len = 0;
static bool    s_sdr_ready_sent = false;

/* Current LO state */
static uint32_t s_last_hz   = 0;
static char     s_last_type = 'X';

/* ---- Helpers ------------------------------------------------------ */
static void cdc_write(const char *s)
{
    tud_cdc_write_str(s);
    tud_cdc_write_flush();
}

static void configure_gpios_for_adc(bool is_cjc5430)
{
    if (is_cjc5430) {
        /* Configure CJC5430 pins (GPIO 6, 7, 8) for PIO function */
        for (int pin = 6; pin <= 8; pin++) {
            pio_gpio_init(pio0, pin);
            gpio_pull_up(pin);
        }
        /* Set input direction for PIO on GPIO 6, 7, 8 */
        pio_sm_set_consecutive_pindirs(pio0, 0, 6, 3, false);

        /* Reset PCM1808 pins (GPIO 9, 10, 11) to standard GPIO input with pull-ups */
        for (int pin = 9; pin <= 11; pin++) {
            gpio_init(pin);
            gpio_set_dir(pin, GPIO_IN);
            gpio_pull_up(pin);
        }
    } else {
        /* Configure PCM1808 pins (GPIO 9, 10, 11) for PIO function */
        for (int pin = 9; pin <= 11; pin++) {
            pio_gpio_init(pio0, pin);
            gpio_pull_up(pin);
        }
        /* Set input direction for PIO on GPIO 9, 10, 11 */
        pio_sm_set_consecutive_pindirs(pio0, 0, 9, 3, false);

        /* Reset CJC5430 pins (GPIO 6, 7, 8) to standard GPIO input with pull-ups */
        for (int pin = 6; pin <= 8; pin++) {
            gpio_init(pin);
            gpio_set_dir(pin, GPIO_IN);
            gpio_pull_up(pin);
        }
    }
}

/* ---- Forward declarations ----------------------------------------- */
static void cdc_task(void);
static void handle_line(const char *line, uint8_t len);
static void apply_sample_rate(uint32_t rate);

/* ======================================================================
 * DMA IRQ0 handler -- runs on Core 1
 * ====================================================================== */
static void __not_in_flash_func(dma_handler)(void)
{
    const uint32_t *filled_buf;
    uint32_t wpb = g_words_per_buf;

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

    /* Non-blocking push */
    multicore_fifo_push_timeout_us((uint32_t)(uintptr_t)filled_buf, 0);
}

/* ---- Core 1 entry ------------------------------------------------- */
static void core1_entry(void)
{
    multicore_fifo_drain();

    dma_channel_set_irq0_enabled(dma_chan_a, true);
    dma_channel_set_irq0_enabled(dma_chan_b, true);

    irq_set_exclusive_handler(DMA_IRQ_0, dma_handler);
    irq_set_enabled(DMA_IRQ_0, true);

    dma_channel_start(dma_chan_a);

    while (true) {
        uint32_t msg;
        if (multicore_fifo_pop_timeout_us(0, &msg)) {
            if (msg == 0xFFFFFFFFu) {
                irq_set_enabled(DMA_IRQ_0, false);
                dma_channel_abort(dma_chan_a);
                dma_channel_abort(dma_chan_b);

                /* Align PIO so the first word captured is Left */
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
        }
        tight_loop_contents();
    }
}

/* ======================================================================
 * apply_sample_rate -- Core 0
 * ====================================================================== */
static void apply_sample_rate(uint32_t rate)
{
    if (rate != 48000U && rate != 96000U && rate != 192000U) return;

    /* PCM1808 does not support 192 kHz. */
    if (g_adc_type == ADC_PCM1808 && rate == 192000U) {
        g_adc_type = ADC_CJC5430;
    }

    g_words_per_buf = (rate == 192000U) ? 384u : ((rate == 96000U) ? 192u : 96u);
    g_sample_rate   = rate;

    /* Set up hardware mode selection pins M0 (GPIO 22) and M1 (GPIO 26) */
    if (g_adc_type == ADC_PCM1808) {
        /* PCM1808 MD0=0 (I2S standard format), MD1=0 (48k) / 1 (96k) */
        gpio_put(MODE_M0_PIN, 0);
        gpio_put(MODE_M1_PIN, (rate == 96000U) ? 1 : 0);
    } else {
        /* CJC5430 M0=0 (48k/192k) / 1 (96k), M1=0 (48k/96k) / 1 (192k) */
        gpio_put(MODE_M0_PIN, (rate == 96000U) ? 1 : 0);
        gpio_put(MODE_M1_PIN, (rate == 192000U) ? 1 : 0);
    }

    /* Reconfigure PIO pins and state machine */
    pio_sm_set_enabled(pio0, 0, false);
    configure_gpios_for_adc(g_adc_type == ADC_CJC5430);

    pio_sm_config c = i2s_rx_program_get_default_config(g_pio_offset);
    sm_config_set_in_pins(&c, (g_adc_type == ADC_CJC5430) ? 6 : 9);
    sm_config_set_in_shift(&c, false, true, 32);
    sm_config_set_clkdiv(&c, 1.0f);
    pio_sm_init(pio0, 0, g_pio_offset, &c);

    /* Signal Core 1 to restart DMA with new trans_count */
    multicore_fifo_push_blocking(0xFFFFFFFFu);

    /* Wait for Core 1 ack */
    uint32_t ack = 0;
    while (!multicore_fifo_pop_timeout_us(100000, &ack) || ack != 0xFFFFFFFEu) {
        tight_loop_contents();
    }
}

/* ======================================================================
 * audio_task -- runs on Core 0
 * ====================================================================== */
static void audio_task(void)
{
    static uint8_t packed[MAX_WORDS_PER_BUF * 3];

    uint32_t ptr_val = 0;
    if (!multicore_fifo_pop_timeout_us(0, &ptr_val) || ptr_val == 0) return;

    const uint32_t *src = (const uint32_t *)(uintptr_t)ptr_val;
    uint32_t wpb = g_words_per_buf;
    uint32_t bytes_to_write = 0;

    if (g_sample_rate == 192000U) {
        /* 192 kHz: 16-bit dithered stereo (S16_LE) */
        for (uint32_t i = 0; i < wpb; i++) {
            int32_t s24 = ((int32_t)(src[i] << 1)) >> 8;

            int32_t tpdf = (int32_t)((xorshift32_next() >> 24) & 0xFF)
                         - (int32_t)((xorshift32_next() >> 24) & 0xFF);
            int32_t s16 = (s24 + tpdf + 128) >> 8;

            if (s16 > 32767) s16 = 32767;
            if (s16 < -32768) s16 = -32768;

            packed[2*i+0] = (uint8_t)(s16 & 0xFF);
            packed[2*i+1] = (uint8_t)((uint32_t)s16 >> 8);
        }
        bytes_to_write = wpb * 2u;
    } else {
        /* 48/96 kHz: 24-bit stereo (S24_3LE) */
        for (uint32_t i = 0; i < wpb; i++) {
            uint32_t w = src[i] << 1;
            packed[3*i+0] = (uint8_t)(w >>  8);
            packed[3*i+1] = (uint8_t)(w >> 16);
            packed[3*i+2] = (uint8_t)(w >> 24);
        }
        bytes_to_write = wpb * 3u;
    }

    tu_fifo_t *ff = tud_audio_get_ep_in_ff();

    if (ff && tu_fifo_remaining(ff) >= bytes_to_write) {
        tud_audio_write(packed, bytes_to_write);
    } else {
        /* Drop this 1ms frame entirely rather than misaligning the FIFO! */
    }
}

/* ======================================================================
 * UAC1 Audio Callbacks
 * ====================================================================== */
static uint8_t s_audio_alt = 0;
static uint32_t s_sample_rate_hz = 48000U;
static uint8_t s_mute[3] = {0, 0, 0};

bool tud_audio_set_itf_cb(uint8_t rhport,
                           tusb_control_request_t const *p_request)
{
    (void)rhport;
    s_audio_alt = (uint8_t)(p_request->wValue & 0xFF);

    static const uint8_t silence[3072] = {0};
    if (s_audio_alt == 1u) {
        apply_sample_rate(48000U);
        tud_audio_write(silence, 576u);
    } else if (s_audio_alt == 2u) {
        apply_sample_rate(96000U);
        tud_audio_write(silence, 1152u);
    } else if (s_audio_alt == 3u) {
        apply_sample_rate(192000U);
        tud_audio_write(silence, 3072u);
    }
    return true;
}

bool tud_audio_set_itf_close_ep_cb(uint8_t rhport,
                                    tusb_control_request_t const *p_request)
{
    (void)rhport; (void)p_request;
    return true;
}

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
        if (rate == 48000U || rate == 96000U || rate == 192000U) {
            g_pending_rate = rate;
            if (s_audio_alt > 0) {
                apply_sample_rate(rate);
            }
        }
        s_sample_rate_hz = rate;
    }
    return true;
}

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
    if (dtr && !s_sdr_ready_sent) {
        cdc_write("SDR ready - waiting for precise integer commands...\r\n");
        s_sdr_ready_sent = true;
    }
    if (!dtr) {
        s_sdr_ready_sent = false;
        s_line_len = 0;
    }
}

void tud_cdc_rx_cb(uint8_t itf) { (void)itf; }

static void cdc_task(void)
{
    if (!tud_cdc_connected()) {
        s_line_len = 0;
        return;
    }

    while (tud_cdc_available()) {
        uint8_t ch;
        tud_cdc_read(&ch, 1);

        if (ch == 0x03) { s_line_len = 0; continue; }

        if (ch == 0x04) {
            s_line_len = 0;
            g_adc_type = ADC_PCM1808;
            g_pending_rate = 48000U;
            apply_sample_rate(48000U);
            cdc_write("SDR ready - waiting for precise integer commands...\r\n");
            continue;
        }

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

static void handle_line(const char *line, uint8_t len)
{
    if (len == 0) return;

    char reply[96];

    /* VER */
    if (strncmp(line, "VER", 3) == 0) {
        cdc_write("VER,SDR C firmware 2.0 (Multi-ADC)\r\nOK\r\n");
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
            if (rate == 48000U || rate == 96000U || rate == 192000U) {
                g_pending_rate = rate;
                apply_sample_rate(rate);
            }
        } else {
            snprintf(reply, sizeof(reply), "RATE,%lu\r\n", (unsigned long)g_sample_rate);
            cdc_write(reply);
        }
        cdc_write("OK\r\n");
        return;
    }

    /* ADC (query or set) */
    if (strncmp(line, "ADC", 3) == 0) {
        if (line[3] == ',') {
            adc_type_t new_adc = g_adc_type;
            if (strcmp(line + 4, "PCM1808") == 0) {
                new_adc = ADC_PCM1808;
            } else if (strcmp(line + 4, "CJC5430") == 0) {
                new_adc = ADC_CJC5430;
            } else {
                cdc_write("ERR\r\n");
                return;
            }
            if (new_adc != g_adc_type) {
                g_adc_type = new_adc;
                uint32_t rate = g_sample_rate;
                if (g_adc_type == ADC_PCM1808 && rate == 192000U) {
                    rate = 96000U;
                    g_pending_rate = 96000U;
                }
                apply_sample_rate(rate);
            }
        } else {
            snprintf(reply, sizeof(reply), "ADC,%s\r\n", (g_adc_type == ADC_CJC5430) ? "CJC5430" : "PCM1808");
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

    /* FREQ,<hz>,<N>,<a>,<b>,<c>,<P1>,<P2>,<P3> */
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

/* ---- Main -------------------------------------------------------- */
int main(void)
{
    board_init();

    vreg_set_voltage(VREG_VOLTAGE_1_20);
    set_sys_clock_khz(250000, true);

    /* ---- Control GPIOs -------------------------------------------- */
    gpio_init(MODE_M0_PIN);
    gpio_set_dir(MODE_M0_PIN, GPIO_OUT);
    gpio_put(MODE_M0_PIN, 0);

    gpio_init(MODE_M1_PIN);
    gpio_set_dir(MODE_M1_PIN, GPIO_OUT);
    gpio_put(MODE_M1_PIN, 0);

    /* Initial state: PCM1808 at 48 kHz */
    g_adc_type      = ADC_PCM1808;
    g_words_per_buf = 96u;
    g_sample_rate   = 48000U;
    g_pending_rate  = 48000U;

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

    /* Initial GPIO configuration for PCM1808 */
    configure_gpios_for_adc(false);

    pio_sm_config c = i2s_rx_program_get_default_config(g_pio_offset);
    sm_config_set_in_pins(&c, 9); /* PCM1808 base is 9 */
    sm_config_set_in_shift(&c, /*shift_direction=*/false,
                                /*autopush=*/true,
                                /*push_threshold=*/32);
    sm_config_set_clkdiv(&c, 1.0f);
    pio_sm_init(pio, sm, g_pio_offset, &c);
    pio_sm_set_enabled(pio, sm, true);

    /* ---- DMA ping-pong -------------------------------------------- */
    dma_chan_a = dma_claim_unused_channel(true);
    dma_chan_b = dma_claim_unused_channel(true);

    dma_channel_config ca = dma_channel_get_default_config(dma_chan_a);
    channel_config_set_transfer_data_size(&ca, DMA_SIZE_32);
    channel_config_set_read_increment(&ca,  false);
    channel_config_set_write_increment(&ca, true);
    channel_config_set_dreq(&ca, pio_get_dreq(pio, sm, false));
    channel_config_set_chain_to(&ca, dma_chan_b);
    dma_channel_configure(dma_chan_a, &ca,
                          buf_a, &pio->rxf[sm], g_words_per_buf, false);

    dma_channel_config cb = dma_channel_get_default_config(dma_chan_b);
    channel_config_set_transfer_data_size(&cb, DMA_SIZE_32);
    channel_config_set_read_increment(&cb,  false);
    channel_config_set_write_increment(&cb, true);
    channel_config_set_dreq(&cb, pio_get_dreq(pio, sm, false));
    channel_config_set_chain_to(&cb, dma_chan_a);
    dma_channel_configure(dma_chan_b, &cb,
                          buf_b, &pio->rxf[sm], g_words_per_buf, false);

    /* ---- Launch Core 1 ---- */
    multicore_launch_core1(core1_entry);

    /* ---- TinyUSB init --------------------------------------------- */
    tusb_rhport_init_t dev_init = {
        .role  = TUSB_ROLE_DEVICE,
        .speed = TUSB_SPEED_AUTO
    };
    tusb_init(BOARD_TUD_RHPORT, &dev_init);

    while (true) {
        tud_task();
        cdc_task();
        audio_task();
    }
}
