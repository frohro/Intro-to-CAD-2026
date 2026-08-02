/*
 * CJC4334H I2S DAC & PAM8908 Headphone Amplifier Test Firmware
 * Board: Intro-to-CAD-2026 (YD-RP2040 v0.1 / v0.2)
 *
 * Signal Architecture:
 *   - Continuous Zero-Gap DMA Ping-Pong Chaining (IRQ driven)
 *   - Cubic Perceptual Volume Control (Whisper-quiet at 5%, comfortable listening)
 *   - Continuous Phase Direct Digital Synthesis (Zero phase jumps, pure sinusoids)
 *   - Sample Rate: 48,000 Hz, BCLK: 3.072 MHz (64fs), MCLK: 24.576 MHz
 *
 * Hardware Connections:
 *   - GP16: I2S Bit Clock (SCK / BCLK)  [JP34]
 *   - GP17: I2S Word Select (WS / LRCK) [JP33]
 *   - GP18: I2S Serial Data (SD / DOUT) [JP32]
 *   - GP15: Optional MCLK clock output (JP6 2-3)
 *   - J9:   3.5mm Headphone Jack (driven by PAM8908 stereo amplifier)
 */

#include <stdio.h>
#include <math.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/clocks.h"
#include "hardware/pwm.h"
#include "hardware/irq.h"

#include "i2s_tx.pio.h"

#define PIN_MCLK        15
#define PIN_SCK         16
#define PIN_WS          17
#define PIN_SD          18

#define SAMPLE_RATE     48000
#define SINE_LUT_SIZE   256
#define SAMPLES_PER_BUF 256
#define WORDS_PER_BUF   (SAMPLES_PER_BUF * 2)

// Test Modes
typedef enum {
    MODE_1KHZ_SINE = 1,
    MODE_STEREO_PAN,
    MODE_FREQ_SWEEP,
    MODE_POLYPHONIC_CHORD,
    MODE_MUTE
} test_mode_t;

// Global state
static int16_t sine_table[SINE_LUT_SIZE];
static uint32_t audio_buffer[2][WORDS_PER_BUF];

static test_mode_t current_mode = MODE_1KHZ_SINE;
static float master_volume = 0.05f;     // Default 5% volume (comfortable for PAM8908 amp)
static float base_frequency = 1000.0f;   // 1 kHz default
static bool gp15_mclk_enabled = false;

// DDS phase accumulators (fixed point 16.16) - preserved continuously across buffer refills
static uint32_t phase_acc_l = 0;
static uint32_t phase_acc_r = 0;
static uint32_t pan_timer = 0;
static bool pan_left = true;
static float sweep_freq = 20.0f;
static uint32_t chord_acc1 = 0, chord_acc2 = 0, chord_acc3 = 0;

// DMA Ping-Pong Channels
static int dma_chan_a = -1;
static int dma_chan_b = -1;
static PIO i2s_pio = pio0;
static uint i2s_sm = 0;

// Pre-calculate 16-bit sine lookup table
static void init_sine_table(void) {
    for (int i = 0; i < SINE_LUT_SIZE; i++) {
        double rad = (2.0 * M_PI * i) / SINE_LUT_SIZE;
        sine_table[i] = (int16_t)(sin(rad) * 32767.0);
    }
}

// Generate next audio buffer with exact phase continuity
static void fill_audio_buffer(uint32_t *buf, size_t sample_count) {
    // Cubic perceptual volume scaling for smooth headphone control
    // master_volume 0.05 (5%) -> vol_scale = 0.000125 (soft & clean on PAM8908 amp)
    float vol_scale = master_volume * master_volume * master_volume;

    for (size_t i = 0; i < sample_count; i++) {
        int16_t sample_l = 0;
        int16_t sample_r = 0;

        switch (current_mode) {
            case MODE_1KHZ_SINE: {
                // 1 kHz Stereo Sine Wave with smooth phase accumulation
                uint32_t phase_inc = (uint32_t)((base_frequency * SINE_LUT_SIZE * 65536.0f) / SAMPLE_RATE);
                phase_acc_l += phase_inc;
                phase_acc_r += phase_inc;

                uint8_t idx_l = (phase_acc_l >> 16) & (SINE_LUT_SIZE - 1);
                uint8_t idx_r = (phase_acc_r >> 16) & (SINE_LUT_SIZE - 1);

                sample_l = (int16_t)(sine_table[idx_l] * vol_scale);
                sample_r = (int16_t)(sine_table[idx_r] * vol_scale);
                break;
            }

            case MODE_STEREO_PAN: {
                // Alternating L (440 Hz) and R (880 Hz) tone
                pan_timer++;
                if (pan_timer >= SAMPLE_RATE) { // Switch every 1 second
                    pan_timer = 0;
                    pan_left = !pan_left;
                }

                if (pan_left) {
                    uint32_t inc_l = (uint32_t)((440.0f * SINE_LUT_SIZE * 65536.0f) / SAMPLE_RATE);
                    phase_acc_l += inc_l;
                    uint8_t idx = (phase_acc_l >> 16) & (SINE_LUT_SIZE - 1);
                    sample_l = (int16_t)(sine_table[idx] * vol_scale);
                    sample_r = 0;
                } else {
                    uint32_t inc_r = (uint32_t)((880.0f * SINE_LUT_SIZE * 65536.0f) / SAMPLE_RATE);
                    phase_acc_r += inc_r;
                    uint8_t idx = (phase_acc_r >> 16) & (SINE_LUT_SIZE - 1);
                    sample_l = 0;
                    sample_r = (int16_t)(sine_table[idx] * vol_scale);
                }
                break;
            }

            case MODE_FREQ_SWEEP: {
                // Logarithmic frequency sweep 20 Hz -> 20,000 Hz
                sweep_freq *= 1.00015f; // Exponential growth
                if (sweep_freq > 20000.0f) sweep_freq = 20.0f;

                uint32_t inc = (uint32_t)((sweep_freq * SINE_LUT_SIZE * 65536.0f) / SAMPLE_RATE);
                phase_acc_l += inc;
                phase_acc_r += inc;

                uint8_t idx = (phase_acc_l >> 16) & (SINE_LUT_SIZE - 1);
                sample_l = (int16_t)(sine_table[idx] * vol_scale);
                sample_r = sample_l;
                break;
            }

            case MODE_POLYPHONIC_CHORD: {
                // A-Major triad: A4 (440 Hz), C#5 (554.37 Hz), E5 (659.25 Hz)
                chord_acc1 += (uint32_t)((440.00f * SINE_LUT_SIZE * 65536.0f) / SAMPLE_RATE);
                chord_acc2 += (uint32_t)((554.37f * SINE_LUT_SIZE * 65536.0f) / SAMPLE_RATE);
                chord_acc3 += (uint32_t)((659.25f * SINE_LUT_SIZE * 65536.0f) / SAMPLE_RATE);

                int32_t mix = sine_table[(chord_acc1 >> 16) & (SINE_LUT_SIZE - 1)] +
                              sine_table[(chord_acc2 >> 16) & (SINE_LUT_SIZE - 1)] +
                              sine_table[(chord_acc3 >> 16) & (SINE_LUT_SIZE - 1)];

                sample_l = (int16_t)((mix / 3) * vol_scale);
                sample_r = sample_l;
                break;
            }

            case MODE_MUTE:
            default:
                sample_l = 0;
                sample_r = 0;
                break;
        }

        // Format 32-bit I2S words for Left and Right channels:
        // Bit 31: 0 (dummy bit for 1-bit I2S delay)
        // Bits 30..15: 16-bit audio sample
        // Bits 14..0: 0 (padding bits)
        buf[2 * i + 0] = ((uint32_t)(uint16_t)sample_l) << 15;
        buf[2 * i + 1] = ((uint32_t)(uint16_t)sample_r) << 15;
    }
}

// Zero-gap DMA Interrupt Handler for Ping-Pong Buffers
static void dma_irq_handler(void) {
    if (dma_channel_get_irq0_status(dma_chan_a)) {
        dma_channel_acknowledge_irq0(dma_chan_a);
        // Buffer 0 finished transferring -> refill Buffer 0 while Buffer 1 is playing
        fill_audio_buffer(audio_buffer[0], SAMPLES_PER_BUF);
        // Re-arm Channel A read address for next chain
        dma_channel_set_read_addr(dma_chan_a, audio_buffer[0], false);
    }
    if (dma_channel_get_irq0_status(dma_chan_b)) {
        dma_channel_acknowledge_irq0(dma_chan_b);
        // Buffer 1 finished transferring -> refill Buffer 1 while Buffer 0 is playing
        fill_audio_buffer(audio_buffer[1], SAMPLES_PER_BUF);
        // Re-arm Channel B read address for next chain
        dma_channel_set_read_addr(dma_chan_b, audio_buffer[1], false);
    }
}

// Optional GP15 MCLK generator via PWM (12.288 MHz clock output)
static void set_gp15_mclk(bool enable) {
    if (enable) {
        gpio_set_function(PIN_MCLK, GPIO_FUNC_PWM);
        uint slice_num = pwm_gpio_to_slice_num(PIN_MCLK);
        pwm_set_wrap(slice_num, 9);
        pwm_set_chan_level(slice_num, pwm_gpio_to_channel(PIN_MCLK), 5);
        pwm_set_enabled(slice_num, true);
        gp15_mclk_enabled = true;
        printf("[MCLK] Enabled GP15 clock output (~12.5 MHz PWM).\n");
    } else {
        gpio_set_function(PIN_MCLK, GPIO_FUNC_SIO);
        gpio_set_dir(PIN_MCLK, GPIO_IN);
        gp15_mclk_enabled = false;
        printf("[MCLK] Disabled GP15 clock output (Assuming 24.576 MHz crystal oscillator input).\n");
    }
}

// Print USB CDC Terminal Interface Menu
static void print_menu(void) {
    printf("\033[2J\033[H"); // Clear terminal screen
    printf("======================================================\n");
    printf("   CJC4334H DAC & PAM8908 Headphone Amp Test Suite    \n");
    printf("   Board: Intro-to-CAD-2026 (YD-RP2040 v0.1 / v0.2)   \n");
    printf("======================================================\n");
    printf(" System Clock: %.3f MHz\n", clock_get_hz(clk_sys) / 1e6f);
    printf(" MCLK Source : %s\n", gp15_mclk_enabled ? "GP15 PWM (12.288 MHz)" : "Onboard 24.576 MHz Oscillator (JP6 Pin 1-2)");
    printf(" Current Mode: %d (", current_mode);
    switch (current_mode) {
        case MODE_1KHZ_SINE:       printf("1 kHz Stereo Sine Wave"); break;
        case MODE_STEREO_PAN:      printf("Stereo L/R Channel Test"); break;
        case MODE_FREQ_SWEEP:       printf("20 Hz - 20 kHz Freq Sweep"); break;
        case MODE_POLYPHONIC_CHORD: printf("A-Major Polyphonic Chord"); break;
        case MODE_MUTE:            printf("Muted / Silence"); break;
    }
    printf(")\n");
    printf(" Frequency   : %.1f Hz\n", base_frequency);
    printf(" Master Vol  : %.1f%% (Cubic Scaled for PAM8908 Amp)\n", master_volume * 100.0f);
    printf(" I2S Pins    : BCLK=GP16 (3.072MHz), WS=GP17 (48.0kHz), SD=GP18\n");
    printf(" Streaming   : Zero-Gap Hardware DMA Ping-Pong\n");
    printf("------------------------------------------------------\n");
    printf(" Controls:\n");
    printf("   [1] 1 kHz Stereo Sine Wave\n");
    printf("   [2] Stereo L/R Channel Separation Test (440Hz L / 880Hz R)\n");
    printf("   [3] Logarithmic Frequency Sweep (20Hz - 20kHz)\n");
    printf("   [4] Polyphonic A-Major Chord\n");
    printf("   [5] Mute Audio Output\n");
    printf("   [+] Increase Volume (+1%%)\n");
    printf("   [-] Decrease Volume (-1%%)\n");
    printf("   [*] Increase Volume (+5%%)\n");
    printf("   [/] Decrease Volume (-5%%)\n");
    printf("   [f] Increase Base Frequency (+50 Hz)\n");
    printf("   [F] Decrease Base Frequency (-50 Hz)\n");
    printf("   [m] Toggle GP15 MCLK Generation\n");
    printf("   [h] Reprint Menu / Clear Screen\n");
    printf("======================================================\n");
    printf("Press any key in minicom to update settings...\n\n");
}

int main() {
    stdio_init_all();
    init_sine_table();

    // Heartbeat onboard LED (GP25 on Pico / YD-RP2040)
    const uint LED_PIN = PICO_DEFAULT_LED_PIN;
    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);
    gpio_put(LED_PIN, 1);

    // Initialize PIO state machine for I2S output
    i2s_pio = pio0;
    i2s_sm = pio_claim_unused_sm(i2s_pio, true);
    uint offset = pio_add_program(i2s_pio, &i2s_tx_program);
    i2s_tx_program_init(i2s_pio, i2s_sm, offset, PIN_SD, PIN_SCK, PIN_WS, SAMPLE_RATE);

    // Claim two DMA channels for ping-pong double buffering
    dma_chan_a = dma_claim_unused_channel(true);
    dma_chan_b = dma_claim_unused_channel(true);

    // Pre-fill both audio buffers with initial smooth audio data
    fill_audio_buffer(audio_buffer[0], SAMPLES_PER_BUF);
    fill_audio_buffer(audio_buffer[1], SAMPLES_PER_BUF);

    // Configure DMA Channel A -> Transfers Buffer 0, then chains to Channel B
    dma_channel_config cfg_a = dma_channel_get_default_config(dma_chan_a);
    channel_config_set_transfer_data_size(&cfg_a, DMA_SIZE_32);
    channel_config_set_read_increment(&cfg_a, true);
    channel_config_set_write_increment(&cfg_a, false);
    channel_config_set_dreq(&cfg_a, pio_get_dreq(i2s_pio, i2s_sm, true));
    channel_config_set_chain_to(&cfg_a, dma_chan_b);

    dma_channel_configure(
        dma_chan_a,
        &cfg_a,
        &i2s_pio->txf[i2s_sm],
        audio_buffer[0],
        WORDS_PER_BUF,
        false
    );

    // Configure DMA Channel B -> Transfers Buffer 1, then chains back to Channel A
    dma_channel_config cfg_b = dma_channel_get_default_config(dma_chan_b);
    channel_config_set_transfer_data_size(&cfg_b, DMA_SIZE_32);
    channel_config_set_read_increment(&cfg_b, true);
    channel_config_set_write_increment(&cfg_b, false);
    channel_config_set_dreq(&cfg_b, pio_get_dreq(i2s_pio, i2s_sm, true));
    channel_config_set_chain_to(&cfg_b, dma_chan_a);

    dma_channel_configure(
        dma_chan_b,
        &cfg_b,
        &i2s_pio->txf[i2s_sm],
        audio_buffer[1],
        WORDS_PER_BUF,
        false
    );

    // Enable IRQ0 for both DMA channels
    dma_set_irq0_channel_mask_enabled((1u << dma_chan_a) | (1u << dma_chan_b), true);
    irq_set_exclusive_handler(DMA_IRQ_0, dma_irq_handler);
    irq_set_enabled(DMA_IRQ_0, true);

    // Kick off hardware DMA streaming (Channel A starts, automatically loops forever with 0 gap)
    dma_channel_start(dma_chan_a);

    uint32_t last_heartbeat = 0;
    bool initial_menu_printed = false;

    while (1) {
        // Heartbeat LED toggle every 500 ms
        uint32_t now = to_ms_since_boot(get_absolute_time());
        if (now - last_heartbeat > 500) {
            last_heartbeat = now;
            gpio_xor_mask(1u << LED_PIN);
            
            // Print initial menu once stdio is connected
            if (stdio_usb_connected() && !initial_menu_printed) {
                initial_menu_printed = true;
                print_menu();
            }
        }

        // Process USB Serial UART CLI input
        int c = getchar_timeout_us(0);
        if (c != PICO_ERROR_TIMEOUT) {
            switch (c) {
                case '1':
                    current_mode = MODE_1KHZ_SINE;
                    printf(">> Selected: 1 kHz Stereo Sine Wave\n");
                    break;
                case '2':
                    current_mode = MODE_STEREO_PAN;
                    printf(">> Selected: Stereo L/R Channel Separation Test\n");
                    break;
                case '3':
                    current_mode = MODE_FREQ_SWEEP;
                    sweep_freq = 20.0f;
                    printf(">> Selected: 20 Hz - 20 kHz Frequency Sweep\n");
                    break;
                case '4':
                    current_mode = MODE_POLYPHONIC_CHORD;
                    printf(">> Selected: Polyphonic A-Major Chord\n");
                    break;
                case '5':
                    current_mode = MODE_MUTE;
                    printf(">> Selected: Muted / Silence\n");
                    break;
                case '+':
                case '=':
                    master_volume += 0.01f;
                    if (master_volume > 1.0f) master_volume = 1.0f;
                    printf(">> Volume: %.1f%%\n", master_volume * 100.0f);
                    break;
                case '-':
                case '_':
                    master_volume -= 0.01f;
                    if (master_volume < 0.0f) master_volume = 0.0f;
                    printf(">> Volume: %.1f%%\n", master_volume * 100.0f);
                    break;
                case '*':
                    master_volume += 0.05f;
                    if (master_volume > 1.0f) master_volume = 1.0f;
                    printf(">> Volume: %.1f%%\n", master_volume * 100.0f);
                    break;
                case '/':
                    master_volume -= 0.05f;
                    if (master_volume < 0.0f) master_volume = 0.0f;
                    printf(">> Volume: %.1f%%\n", master_volume * 100.0f);
                    break;
                case 'f':
                    base_frequency += 50.0f;
                    if (base_frequency > 20000.0f) base_frequency = 20000.0f;
                    printf(">> Base Frequency: %.1f Hz\n", base_frequency);
                    break;
                case 'F':
                    base_frequency -= 50.0f;
                    if (base_frequency < 20.0f) base_frequency = 20.0f;
                    printf(">> Base Frequency: %.1f Hz\n", base_frequency);
                    break;
                case 'm':
                case 'M':
                    set_gp15_mclk(!gp15_mclk_enabled);
                    break;
                case 'h':
                case 'H':
                case '?':
                case '\r':
                case '\n':
                    print_menu();
                    break;
                default:
                    print_menu();
                    break;
            }
        }

        tight_loop_contents();
    }

    return 0;
}
