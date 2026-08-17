/*
 * main.c -- Unified RP2040 / Pico W SDR Firmware (v0.2)
 *
 * Features:
 *   • Dual Streaming: USB Audio Class 1.0 (UAC1) + OpenHPSDR Protocol 1 over WiFi
 *   • Autonomous On-Board Si5351 LO calculation (Golden Integer & Fractional fallback)
 *   • Dual FREQ command support:
 *       1. Legacy Quisk format: FREQ,<hz>,<N>,<a>,<b>,<c>,<P1>,<P2>,<P3>
 *       2. Direct frequency:    FREQ,<hz>  (computes best LO on-chip)
 *   • Sample Rate switching: 48 kHz (M1=0) / 96 kHz (M1=1) dynamically
 *   • TCP Control Server on Port 5000 in parallel with USB CDC
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "bsp/board_api.h"
#include "tusb.h"

#include "hardware/vreg.h"
#include "hardware/clocks.h"
#include "hardware/i2c.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/multicore.h"
#include "pico/cyw43_arch.h"
#include "lwip/tcp.h"

#include "i2s_rx.pio.h"
#include "si5351.h"
#include "openhpsdr.h"
#include "wifi_config.h"

/* GPIO pin assignments */
#define I2C_SDA_PIN    12
#define I2C_SCL_PIN    13
#define MODE_M0_PIN    22   /* PCM1808 MD0: held LOW (I2S format) */
#define MODE_M1_PIN    26   /* PCM1808 MD1: 0=48kHz, 1=96kHz      */

#define PCM_DATA_PIN   9
#define PCM_BCK_PIN    10
#define PCM_WS_PIN     11

#define I2C_SPEED_HZ   100000U
#define BOARD_DIRECT_MODE true

#define MAX_WORDS_PER_BUF  192

static uint32_t buf_a[MAX_WORDS_PER_BUF];
static uint32_t buf_b[MAX_WORDS_PER_BUF];

static volatile uint32_t g_words_per_buf = 96;    /* default 48 kHz */
static volatile uint32_t g_sample_rate   = 48000U;
static uint g_pio_offset = 0;

static int dma_chan_a;
static int dma_chan_b;

/* CDC / TCP line buffer */
#define LINE_BUF_LEN  128
static char    s_line[LINE_BUF_LEN];
static uint8_t s_line_len = 0;
static bool    s_sdr_ready_sent = false;

static uint32_t s_last_hz   = 7050000;
static char     s_last_type = 'G';
static lo_calc_mode_t s_lo_mode = LO_MODE_BEST_INTEGER;

/* UAC1 state */
static uint8_t  s_audio_alt      = 0;
static uint32_t s_sample_rate_hz = 48000U;
static uint8_t  s_mute[3]        = {0, 0, 0};

/* Forward declarations */
static void apply_sample_rate(uint32_t rate);
static void handle_line(const char *line, uint8_t len, void (*reply_fn)(const char *));
static void cdc_write(const char *s);

/* WiFi SSID and Password state */
static char s_wifi_ssid[64] = DEFAULT_WIFI_SSID;
static char s_wifi_pass[64] = DEFAULT_WIFI_PASSWORD;

/* TCP Control Server */
static struct tcp_pcb *s_tcp_server_pcb = NULL;
static struct tcp_pcb *s_active_tcp_client = NULL;

static void tcp_write_str(const char *s) {
    if (s_active_tcp_client) {
        tcp_write(s_active_tcp_client, s, (u16_t)strlen(s), TCP_WRITE_FLAG_COPY);
        tcp_output(s_active_tcp_client);
    }
}

/* ======================================================================
 * Core 1: owns DMA IRQ, handles rate-reconfiguration sentinel
 * ====================================================================== */
static void dma_handler(void) {
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
    multicore_fifo_push_timeout_us((uint32_t)(uintptr_t)filled_buf, 0);
}

static void pio_configure_pcm1808(void) {
    pio_gpio_init(pio0, PCM_DATA_PIN);
    gpio_pull_up(PCM_DATA_PIN);
    pio_gpio_init(pio0, PCM_BCK_PIN);
    gpio_pull_up(PCM_BCK_PIN);
    pio_gpio_init(pio0, PCM_WS_PIN);
    gpio_pull_up(PCM_WS_PIN);

    pio_sm_set_consecutive_pindirs(pio0, 0, PCM_DATA_PIN, 3, false);

    pio_sm_config c = i2s_rx_program_get_default_config(g_pio_offset);
    sm_config_set_in_pins(&c, PCM_DATA_PIN);
    sm_config_set_in_shift(&c, false, true, 32);
    sm_config_set_clkdiv(&c, 1.0f);
    pio_sm_init(pio0, 0, g_pio_offset, &c);
}

static void core1_entry(void) {
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

                pio_sm_set_enabled(pio0, 0, false);
                pio_sm_clear_fifos(pio0, 0);

                pio_configure_pcm1808();

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
                multicore_fifo_push_blocking(0xFFFFFFFEu);
            }
        }
        tight_loop_contents();
    }
}

/* ======================================================================
 * Hardware Reconfiguration
 * ====================================================================== */
static void apply_hardware_config(uint32_t rate) {
    gpio_put(MODE_M0_PIN, 0);
    if (rate >= 96000U) {
        gpio_put(MODE_M1_PIN, 1);
        g_words_per_buf = 192;
        g_sample_rate   = 96000U;
    } else {
        gpio_put(MODE_M1_PIN, 0);
        g_words_per_buf = 96;
        g_sample_rate   = 48000U;
    }
}

static void restart_pio_dma(void) {
    multicore_fifo_push_blocking(0xFFFFFFFFu);
    uint32_t ack = 0;
    while (!multicore_fifo_pop_timeout_us(100000, &ack) || ack != 0xFFFFFFFEu)
        tight_loop_contents();
}

static void apply_sample_rate(uint32_t rate) {
    if (rate != 48000U && rate != 96000U) return;
    if (rate == g_sample_rate) return;
    apply_hardware_config(rate);
    restart_pio_dma();
}

/* ======================================================================
 * Audio Stream Tasks (Core 0: USB + OpenHPSDR)
 * ====================================================================== */
void audio_task(void) {
    uint32_t ptr_val = 0;
    while (multicore_fifo_pop_timeout_us(0, &ptr_val)) {
        if (ptr_val == 0) continue;

        const uint32_t *src = (const uint32_t *)(uintptr_t)ptr_val;
        uint32_t wpb = g_words_per_buf;

        // 1. Stream via OpenHPSDR Protocol 1 if network client is connected
        if (openhpsdr_is_active()) {
            openhpsdr_push_samples(src, wpb);
        }

        // 2. Stream via USB Audio Class 1.0 if USB host is listening
        if (tud_audio_mounted() && s_audio_alt > 0) {
            tu_fifo_t *ff = tud_audio_get_ep_in_ff();
            if (ff) {
                static uint8_t packed[MAX_WORDS_PER_BUF * 3];
                for (uint32_t i = 0; i < wpb; i++) {
                    uint32_t w = src[i] << 1;
                    packed[3*i+0] = (uint8_t)(w >>  8);
                    packed[3*i+1] = (uint8_t)(w >> 16);
                    packed[3*i+2] = (uint8_t)(w >> 24);
                }
                uint32_t bytes = wpb * 3u;
                if (tu_fifo_remaining(ff) >= bytes) {
                    tud_audio_write(packed, (uint16_t)bytes);
                }
            }
        }
    }
}

/* ======================================================================
 * OpenHPSDR Callbacks (Deferred execution to protect lwIP & prevent audio stalls)
 * ====================================================================== */
static volatile uint32_t s_pending_freq_hz = 0;
static volatile uint32_t s_pending_rate_hz = 0;

static void on_hpsdr_freq_change(uint32_t freq_hz) {
    s_pending_freq_hz = freq_hz;
}

static void on_hpsdr_rate_change(uint32_t rate_hz) {
    if (rate_hz == 48000U || rate_hz == 96000U) {
        if (rate_hz != g_sample_rate) {
            s_pending_rate_hz = rate_hz;
        }
    }
}

static void service_hpsdr_pending_tuning(void) {
    if (s_pending_rate_hz != 0) {
        uint32_t rate = s_pending_rate_hz;
        s_pending_rate_hz = 0;
        apply_sample_rate(rate);
    }
    if (s_pending_freq_hz != 0) {
        uint32_t freq = s_pending_freq_hz;
        s_pending_freq_hz = 0;
        if (freq != s_last_hz) {
            lo_candidate_t cand;
            if (si5351_tune_frequency(i2c0, freq, g_sample_rate, BOARD_DIRECT_MODE, LO_MODE_FRACTIONAL_EXACT, &cand)) {
                s_last_hz = freq;
                s_last_type = cand.ptype;
            }
        }
    }
}

/* ======================================================================
 * Command Parser (Handles both USB CDC and TCP Sockets)
 * ====================================================================== */
static void cdc_write(const char *s) {
    tud_cdc_write_str(s);
    tud_cdc_write_flush();
}

static void handle_line(const char *raw_line, uint8_t raw_len, void (*reply_fn)(const char *))
{
    if (raw_len == 0) return;
    
    // Trim leading whitespace/newlines
    uint8_t start = 0;
    while (start < raw_len && (raw_line[start] == ' ' || raw_line[start] == '\t' || raw_line[start] == '\r' || raw_line[start] == '\n')) {
        start++;
    }
    if (start >= raw_len) return;

    // Copy and trim trailing whitespace/CR/LF
    char line[LINE_BUF_LEN];
    uint8_t len = 0;
    for (uint8_t i = start; i < raw_len && len < sizeof(line) - 1; i++) {
        char c = raw_line[i];
        if (c != '\r' && c != '\n') {
            line[len++] = c;
        }
    }
    while (len > 0 && (line[len - 1] == ' ' || line[len - 1] == '\t')) {
        len--;
    }
    line[len] = '\0';
    if (len == 0) return;

    // Create uppercase version for case-insensitive matching
    char upper_line[LINE_BUF_LEN];
    for (uint8_t i = 0; i <= len; i++) {
        char c = line[i];
        upper_line[i] = (c >= 'a' && c <= 'z') ? (c - 32) : c;
    }

    char reply[128];

    if (strncmp(upper_line, "VER", 3) == 0) {
        reply_fn("VER,0.2\r\nOK\r\n");
        return;
    }
    if (strncmp(upper_line, "HELP", 4) == 0 || strcmp(upper_line, "?") == 0) {
        reply_fn("Commands: WIFI?, WIFI,ssid,pass, FREQ,hz, RATE,48000, MODE, VER\r\nOK\r\n");
        return;
    }
    if (strncmp(upper_line, "MODE", 4) == 0) {
        reply_fn(BOARD_DIRECT_MODE ? "MODE,DIRECT\r\nOK\r\n" : "MODE,JOHNSON\r\nOK\r\n");
        return;
    }
    // WiFi Status and Configuration: WIFI? or WIFI or WIFI,SSID,PASS
    if (strncmp(upper_line, "WIFI?", 5) == 0 || strcmp(upper_line, "WIFI") == 0) {
        int status = cyw43_wifi_link_status(&cyw43_state, CYW43_ITF_STA);
        const char *st_str = "DOWN";
        if (status == CYW43_LINK_JOIN) st_str = "JOINING";
        else if (status == CYW43_LINK_NOIP) st_str = "NO_IP";
        else if (status == CYW43_LINK_UP) st_str = "CONNECTED";
        else if (status == CYW43_LINK_FAIL) st_str = "AUTH_FAILED";
        else if (status == CYW43_LINK_NONET) st_str = "NO_NETWORK";
        else if (status == CYW43_LINK_BADAUTH) st_str = "BAD_AUTH";

        uint8_t *ip = (uint8_t*)&(cyw43_state.netif[CYW43_ITF_STA].ip_addr.addr);
        snprintf(reply, sizeof(reply), "WIFI,%s,IP:%u.%u.%u.%u,SSID:%s\r\nOK\r\n",
                 st_str, ip[0], ip[1], ip[2], ip[3], s_wifi_ssid);
        reply_fn(reply);
        return;
    }
    if (strncmp(upper_line, "WIFI,", 5) == 0) {
        // Syntax: WIFI,<SSID>,<PASSWORD> (case sensitive SSID/Password preserved from original line)
        char buf[LINE_BUF_LEN];
        strncpy(buf, line + 5, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        char *ssid = strtok(buf, ",");
        char *pass = strtok(NULL, ",");
        if (ssid) {
            strncpy(s_wifi_ssid, ssid, sizeof(s_wifi_ssid) - 1);
            s_wifi_ssid[sizeof(s_wifi_ssid) - 1] = '\0';
            if (pass) {
                strncpy(s_wifi_pass, pass, sizeof(s_wifi_pass) - 1);
                s_wifi_pass[sizeof(s_wifi_pass) - 1] = '\0';
            } else {
                s_wifi_pass[0] = '\0';
            }
            uint32_t auth = (s_wifi_pass[0] == '\0') ? CYW43_AUTH_OPEN : CYW43_AUTH_WPA2_AES_PSK;
            const char *pass_param = (s_wifi_pass[0] == '\0') ? NULL : s_wifi_pass;
            cyw43_arch_wifi_connect_async(s_wifi_ssid, pass_param, auth);
            snprintf(reply, sizeof(reply), "WIFI,CONNECTING,%s\r\nOK\r\n", s_wifi_ssid);
            reply_fn(reply);
        } else {
            reply_fn("ERROR,invalid wifi format\r\n");
        }
        return;
    }
    if (strncmp(upper_line, "RATE,", 5) == 0) {
        uint32_t r = (uint32_t)strtoul(line + 5, NULL, 10);
        if (r == 48000U || r == 96000U) {
            apply_sample_rate(r);
            snprintf(reply, sizeof(reply), "RATE,%lu\r\nOK\r\n", (unsigned long)r);
            reply_fn(reply);
        } else {
            reply_fn("ERROR,unsupported rate\r\n");
        }
        return;
    }
    // LO Mode Selection: LOMODE,BEST | LOMODE,MID | LOMODE,WORST | LOMODE,FRAC
    if (strncmp(upper_line, "LOMODE,", 7) == 0) {
        if (strncmp(upper_line + 7, "BEST", 4) == 0) s_lo_mode = LO_MODE_BEST_INTEGER;
        else if (strncmp(upper_line + 7, "MID", 3) == 0) s_lo_mode = LO_MODE_MIDDLE_INTEGER;
        else if (strncmp(upper_line + 7, "WORST", 5) == 0) s_lo_mode = LO_MODE_WORST_INTEGER;
        else if (strncmp(upper_line + 7, "FRAC", 4) == 0) s_lo_mode = LO_MODE_FRACTIONAL_EXACT;
        reply_fn("OK\r\n");
        return;
    }
    // Query current frequency: FREQ, or FREQ
    if (strcmp(upper_line, "FREQ,") == 0 || strcmp(upper_line, "FREQ") == 0) {
        snprintf(reply, sizeof(reply), "%lu\r\nOK,%c,0\r\n", (unsigned long)s_last_hz, s_last_type);
        reply_fn(reply);
        return;
    }
    // Single-parameter Tune Command: FREQ,<hz>  (e.g., FREQ,7050000)
    if (strncmp(upper_line, "FREQ,", 5) == 0) {
        // Count commas to see if it's direct frequency or full register dump
        int commas = 0;
        for (int i = 0; line[i]; i++) if (line[i] == ',') commas++;

        if (commas == 1) {
            uint32_t target_hz = (uint32_t)strtoul(line + 5, NULL, 10);
            lo_candidate_t cand;
            if (si5351_tune_frequency(i2c0, target_hz, g_sample_rate, BOARD_DIRECT_MODE, s_lo_mode, &cand)) {
                s_last_hz = target_hz;
                s_last_type = cand.ptype;
                snprintf(reply, sizeof(reply), "%lu\r\nOK,%c,%ld\r\n",
                         (unsigned long)cand.actual_hz, cand.ptype, (long)cand.offset_hz);
                reply_fn(reply);
            } else {
                reply_fn("ERR\r\n");
            }
            return;
        }

        // Full 8-parameter register format (Legacy Quisk compatibility)
        char buf[LINE_BUF_LEN];
        strncpy(buf, line, LINE_BUF_LEN - 1);
        buf[LINE_BUF_LEN - 1] = '\0';

        char *tok = strtok(buf, ",");  if (!tok) goto bad_freq;
        tok = strtok(NULL, ",");       if (!tok) goto bad_freq;
        uint32_t hz = (uint32_t)strtoul(tok, NULL, 10);
        tok = strtok(NULL, ",");       if (!tok) goto bad_freq;
        uint32_t N  = (uint32_t)strtoul(tok, NULL, 10);
        tok = strtok(NULL, ",");       if (!tok) goto bad_freq;
        tok = strtok(NULL, ",");       if (!tok) goto bad_freq;
        uint32_t b  = (uint32_t)strtoul(tok, NULL, 10);
        tok = strtok(NULL, ",");       if (!tok) goto bad_freq;
        tok = strtok(NULL, ",");       if (!tok) goto bad_freq;
        uint32_t P1 = (uint32_t)strtoul(tok, NULL, 10);
        tok = strtok(NULL, ",");       if (!tok) goto bad_freq;
        uint32_t P2 = (uint32_t)strtoul(tok, NULL, 10);
        tok = strtok(NULL, ",");       if (!tok) goto bad_freq;
        uint32_t P3 = (uint32_t)strtoul(tok, NULL, 10);

        si5351_set_freq_regs(i2c0, N, P1, P2, P3, BOARD_DIRECT_MODE);

        char ptype = (b == 0) ? 'G' : 'F';
        s_last_hz   = hz;
        s_last_type = ptype;

        snprintf(reply, sizeof(reply), "%lu\r\nOK,%c,0\r\n", (unsigned long)hz, ptype);
        reply_fn(reply);
        return;

    bad_freq:
        reply_fn("ERR\r\n");
        return;
    }

    reply_fn("ERR\r\n");
}

static void cdc_task(void) {
    while (tud_cdc_available()) {
        uint8_t ch;
        tud_cdc_read(&ch, 1);

        // Ctrl-C: cancel line
        if (ch == 0x03) {
            s_line_len = 0;
            continue;
        }

        // Ctrl-D: status prompt (Quisk initialization)
        if (ch == 0x04) {
            s_line_len = 0;
            cdc_write("SDR ready\r\n");
            continue;
        }

        // Enter / Return (Carriage Return or Line Feed)
        if (ch == '\r' || ch == '\n') {
            if (s_line_len > 0) {
                s_line[s_line_len] = '\0';
                handle_line(s_line, s_line_len, cdc_write);
                s_line_len = 0;
            }
            continue;
        }

        // Backspace / Delete
        if (ch == '\b' || ch == 127) {
            if (s_line_len > 0) {
                s_line_len--;
            }
            continue;
        }

        // Printable characters - buffer without local echo
        if (ch >= 32 && ch <= 126) {
            if (s_line_len < LINE_BUF_LEN - 1) {
                s_line[s_line_len++] = (char)ch;
            }
        }
    }
}

/* ======================================================================
 * TCP Control Server Callback Handlers
 * ====================================================================== */
static err_t tcp_client_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
    (void)arg;
    if (!p) {
        tcp_close(tpcb);
        s_active_tcp_client = NULL;
        return ERR_OK;
    }
    s_active_tcp_client = tpcb;
    char line_buf[LINE_BUF_LEN];
    u16_t len = (p->len < LINE_BUF_LEN - 1) ? p->len : LINE_BUF_LEN - 1;
    pbuf_copy_partial(p, line_buf, len, 0);
    line_buf[len] = '\0';

    // Strip trailing \r and \n
    while (len > 0 && (line_buf[len-1] == '\r' || line_buf[len-1] == '\n')) {
        line_buf[--len] = '\0';
    }

    handle_line(line_buf, (uint8_t)len, tcp_write_str);
    tcp_recved(tpcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

static err_t tcp_server_accept(void *arg, struct tcp_pcb *client_pcb, err_t err) {
    (void)arg; (void)err;
    s_active_tcp_client = client_pcb;
    tcp_recv(client_pcb, tcp_client_recv);
    tcp_write_str("SDR ready (WiFi TCP)\r\n");
    return ERR_OK;
}

static void start_tcp_control_server(void) {
    s_tcp_server_pcb = tcp_new();
    if (s_tcp_server_pcb) {
        tcp_bind(s_tcp_server_pcb, IP_ADDR_ANY, TCP_CONTROL_PORT);
        s_tcp_server_pcb = tcp_listen(s_tcp_server_pcb);
        tcp_accept(s_tcp_server_pcb, tcp_server_accept);
    }
}

/* ======================================================================
 * TinyUSB UAC1 Callbacks
 * ====================================================================== */
bool tud_audio_set_itf_cb(uint8_t rhport, tusb_control_request_t const *p_request) {
    (void)rhport;
    s_audio_alt = (uint8_t)(p_request->wValue & 0xFF);
    if (s_audio_alt > 0) {
        static const uint32_t alt_rate[3] = { 0, 48000U, 96000U };
        uint32_t rate = (s_audio_alt <= 2) ? alt_rate[s_audio_alt] : 48000U;
        s_sample_rate_hz = rate;
        apply_sample_rate(rate);

        static const uint8_t silence[582] = {0};
        uint32_t sil = (rate == 96000U) ? 192u*3u : 96u*3u;
        tud_audio_write(silence, (uint16_t)sil);
    }
    return true;
}

bool tud_audio_set_itf_close_ep_cb(uint8_t rhport, tusb_control_request_t const *p_request) {
    (void)rhport; (void)p_request;
    return true;
}

bool tud_audio_set_req_ep_cb(uint8_t rhport, tusb_control_request_t const *p_request, uint8_t *pBuff) {
    (void)rhport;
    if ((uint8_t)(p_request->wValue >> 8) == AUDIO10_EP_CTRL_SAMPLING_FREQ) {
        s_sample_rate_hz = (uint32_t)pBuff[0] | ((uint32_t)pBuff[1] << 8) | ((uint32_t)pBuff[2] << 16);
    }
    return true;
}

bool tud_audio_get_req_ep_cb(uint8_t rhport, tusb_control_request_t const *p_request) {
    (void)rhport;
    if ((uint8_t)(p_request->wValue >> 8) == AUDIO10_EP_CTRL_SAMPLING_FREQ) {
        uint8_t freq[3] = { (uint8_t)(s_sample_rate_hz), (uint8_t)(s_sample_rate_hz >> 8), (uint8_t)(s_sample_rate_hz >> 16) };
        return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, freq, 3);
    }
    return false;
}

bool tud_audio_set_req_entity_cb(uint8_t rhport, tusb_control_request_t const *p_request, uint8_t *pBuff) {
    (void)rhport;
    uint8_t cs = (uint8_t)(p_request->wValue >> 8);
    uint8_t cn = (uint8_t)(p_request->wValue & 0xFF);
    if (cs == AUDIO10_FU_CTRL_MUTE && cn < 3u) s_mute[cn] = pBuff[0];
    return true;
}

bool tud_audio_get_req_entity_cb(uint8_t rhport, tusb_control_request_t const *p_request) {
    (void)rhport;
    uint8_t cs = (uint8_t)(p_request->wValue >> 8);
    uint8_t cn = (uint8_t)(p_request->wValue & 0xFF);
    if (cs == AUDIO10_FU_CTRL_MUTE) {
        uint8_t val = (cn < 3u) ? s_mute[cn] : 0u;
        return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &val, 1);
    }
    return false;
}

void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts) {
    (void)itf; (void)dtr; (void)rts;
}

void tud_cdc_rx_cb(uint8_t itf) { (void)itf; }

/* ======================================================================
 * main
 * ====================================================================== */
int main(void)
{
    /* Initialize TinyUSB / Board */
    board_init();

    tusb_rhport_init_t dev_init = {
        .role  = TUSB_ROLE_DEVICE,
        .speed = TUSB_SPEED_AUTO
    };
    tusb_init(BOARD_TUD_RHPORT, &dev_init);

    /* GPIO Setup for Mode Pins */
    gpio_init(MODE_M0_PIN);
    gpio_set_dir(MODE_M0_PIN, GPIO_OUT);
    gpio_put(MODE_M0_PIN, 0);

    gpio_init(MODE_M1_PIN);
    gpio_set_dir(MODE_M1_PIN, GPIO_OUT);
    gpio_put(MODE_M1_PIN, 0);

    /* PIO I2S Pins */
    for (int pin = PCM_DATA_PIN; pin <= PCM_WS_PIN; pin++) {
        pio_gpio_init(pio0, pin);
        gpio_pull_up(pin);
    }
    pio_sm_set_consecutive_pindirs(pio0, 0, PCM_DATA_PIN, 3, false);

    /* I2C & Si5351a */
    i2c_init(i2c0, I2C_SPEED_HZ);
    gpio_set_function(I2C_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA_PIN);
    gpio_pull_up(I2C_SCL_PIN);
    si5351_init(i2c0);

    // Initial default LO tune to 7.050 MHz
    lo_candidate_t initial_cand;
    si5351_tune_frequency(i2c0, 7050000, 48000, BOARD_DIRECT_MODE, LO_MODE_BEST_INTEGER, &initial_cand);

    /* PIO Setup */
    g_pio_offset = pio_add_program(pio0, &i2s_rx_program);
    pio_sm_config c = i2s_rx_program_get_default_config(g_pio_offset);
    sm_config_set_in_pins(&c, PCM_DATA_PIN);
    sm_config_set_in_shift(&c, false, true, 32);
    sm_config_set_clkdiv(&c, 1.0f);
    pio_sm_init(pio0, 0, g_pio_offset, &c);
    pio_sm_set_enabled(pio0, 0, true);

    /* DMA Setup */
    dma_chan_a = dma_claim_unused_channel(true);
    dma_chan_b = dma_claim_unused_channel(true);

    dma_channel_config ca = dma_channel_get_default_config(dma_chan_a);
    channel_config_set_transfer_data_size(&ca, DMA_SIZE_32);
    channel_config_set_read_increment(&ca, false);
    channel_config_set_write_increment(&ca, true);
    channel_config_set_dreq(&ca, pio_get_dreq(pio0, 0, false));
    channel_config_set_chain_to(&ca, dma_chan_b);
    dma_channel_configure(dma_chan_a, &ca, buf_a, &pio0->rxf[0], g_words_per_buf, false);

    dma_channel_config cb = dma_channel_get_default_config(dma_chan_b);
    channel_config_set_transfer_data_size(&cb, DMA_SIZE_32);
    channel_config_set_read_increment(&cb, false);
    channel_config_set_write_increment(&cb, true);
    channel_config_set_dreq(&cb, pio_get_dreq(pio0, 0, false));
    channel_config_set_chain_to(&cb, dma_chan_a);
    dma_channel_configure(dma_chan_b, &cb, buf_b, &pio0->rxf[0], g_words_per_buf, false);

    apply_hardware_config(48000U);
    multicore_launch_core1(core1_entry);

    /* Initialize Pico W WiFi (CYW43439) */
    bool wifi_ok = false;
    if (cyw43_arch_init() == 0) {
        wifi_ok = true;
        cyw43_arch_enable_sta_mode();
        // Connect attempt with auto-auth detection (Open vs WPA2-PSK)
        uint32_t auth = (s_wifi_pass[0] == '\0') ? CYW43_AUTH_OPEN : CYW43_AUTH_WPA2_AES_PSK;
        const char *pass_param = (s_wifi_pass[0] == '\0') ? NULL : s_wifi_pass;
        cyw43_arch_wifi_connect_async(s_wifi_ssid, pass_param, auth);

        // Initialize OpenHPSDR UDP server and TCP Control
        openhpsdr_init(on_hpsdr_freq_change, on_hpsdr_rate_change);
        start_tcp_control_server();
    }

    uint32_t last_led_poll = 0;
    while (true) {
        tud_task();
        cdc_task();
        audio_task();
        openhpsdr_task();
        service_hpsdr_pending_tuning();
        if (wifi_ok) {
            cyw43_arch_poll();
        }

        // Continuous Heartbeat LED & Auto-reconnect
        uint32_t now_ms = to_ms_since_boot(get_absolute_time());
        if (wifi_ok && (now_ms - last_led_poll >= 250)) {
            last_led_poll = now_ms;
            int st = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
            if (st == CYW43_LINK_UP) {
                // Heartbeat pulse: blink every 500ms (1 Hz) to show healthy main loop
                bool active = openhpsdr_is_active();
                uint32_t period = active ? 125 : 500; // Fast blink when streaming IQ, steady 1 Hz heartbeat when idle
                cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, (now_ms / period) % 2);
            } else if (st == CYW43_LINK_JOIN || st == CYW43_LINK_NOIP) {
                cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, (now_ms / 250) % 2); // Blinking while joining
            } else {
                cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, (now_ms / 1000) % 2); // Slow pulse when down
                // Auto retry connecting every 10 seconds if link is down
                static uint32_t last_reconnect_ms = 0;
                if (now_ms - last_reconnect_ms >= 10000 && strlen(s_wifi_ssid) > 0) {
                    last_reconnect_ms = now_ms;
                    uint32_t auth = (s_wifi_pass[0] == '\0') ? CYW43_AUTH_OPEN : CYW43_AUTH_WPA2_AES_PSK;
                    const char *pass_param = (s_wifi_pass[0] == '\0') ? NULL : s_wifi_pass;
                    cyw43_arch_wifi_connect_async(s_wifi_ssid, pass_param, auth);
                }
            }
        }
    }
}
