#ifndef SI5351_H
#define SI5351_H

#include "hardware/i2c.h"
#include <stdbool.h>
#include <stdint.h>

// Si5351a I2C address (A0 pin tied to GND)
#define SI5351_I2C_ADDR     0x60

// ==============================================================================
// Si5351 Crystal Calibration
// Nominal crystal: 24.576 MHz (24576000 Hz)
//
// Calibration Formula:
//   F_calibrated = F_nominal * (F_reference_true_hz / F_dial_observed_hz)
//
// Calibration for this board (NBS / WWV 15.000000 MHz observed at 14.997522 MHz):
//   F_calibrated = 24576000 * (15000000 / 14997522) = 24580058 Hz (+165.23 ppm)
// ==============================================================================
#ifndef SI5351_XTAL_FREQ
#define SI5351_XTAL_FREQ    24576000UL
#endif

// Si5351a internal VCO operating range (600 MHz to 900 MHz)
#define SI5351_VCO_MIN      600000000ULL
#define SI5351_VCO_MAX      900000000ULL

// Register definitions
#define SI5351_REG_DEV_STATUS   0
#define SI5351_REG_OEB          3
#define SI5351_REG_CLK0_CTRL    16
#define SI5351_REG_CLK1_CTRL    17
#define SI5351_REG_CLK2_CTRL    18
#define SI5351_REG_PLLA_BASE    26
#define SI5351_REG_MS0_BASE     42
#define SI5351_REG_MS1_BASE     50
#define SI5351_REG_MS2_BASE     58
#define SI5351_REG_CLK0_PHOFF   165
#define SI5351_REG_CLK1_PHOFF   166
#define SI5351_REG_CLK2_PHOFF   167
#define SI5351_REG_PLL_RESET    177
#define SI5351_REG_XTAL_LOAD    183

typedef enum {
    LO_MODE_BEST_INTEGER = 0,
    LO_MODE_MIDDLE_INTEGER,
    LO_MODE_WORST_INTEGER,
    LO_MODE_FRACTIONAL_EXACT
} lo_calc_mode_t;

typedef struct {
    uint32_t target_hz;
    uint32_t actual_hz;
    int32_t  offset_hz;
    uint32_t n;
    uint32_t a;
    uint32_t b;
    uint32_t c;
    uint32_t p1;
    uint32_t p2;
    uint32_t p3;
    char     ptype;     // 'G' (Golden Integer) or 'F' (Fractional)
    bool     valid;
} lo_candidate_t;

// Initialization and raw register I/O
bool si5351_init(i2c_inst_t *i2c);
void si5351_write_reg(i2c_inst_t *i2c, uint8_t reg, uint8_t val);
uint8_t si5351_read_reg(i2c_inst_t *i2c, uint8_t reg);

// Calculate LO parameters internally on RP2040 for a given frequency in Hz
bool si5351_calculate_lo(uint32_t target_hz, uint32_t sample_rate, bool johnson_mode,
                         lo_calc_mode_t mode, lo_candidate_t *cand);

// Direct programming with precomputed register values (Quisk compatibility)
void si5351_set_freq_regs(i2c_inst_t *i2c, uint32_t N,
                          uint32_t P1, uint32_t P2, uint32_t P3,
                          bool direct_mode);

// Program using a calculated candidate struct
void si5351_apply_candidate(i2c_inst_t *i2c, const lo_candidate_t *cand, bool direct_mode);

// Reference crystal calibration runtime control
void si5351_set_xtal_freq(uint32_t freq_hz);
uint32_t si5351_get_xtal_freq(void);

// High-level frequency tune: computes best LO on RP2040 and programs Si5351a
bool si5351_tune_frequency(i2c_inst_t *i2c, uint32_t target_hz, uint32_t sample_rate,
                           bool direct_mode, lo_calc_mode_t mode, lo_candidate_t *applied_cand);

#endif // SI5351_H
