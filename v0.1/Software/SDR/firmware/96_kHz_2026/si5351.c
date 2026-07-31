#include "si5351.h"
#include "hardware/i2c.h"
#include "pico/stdlib.h"
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>   // abs()

// ---------------------------------------------------------------------------
// Register-level helpers
// ---------------------------------------------------------------------------

void si5351_write_reg(i2c_inst_t *i2c, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    i2c_write_blocking(i2c, SI5351_I2C_ADDR, buf, 2, false);
}

uint8_t si5351_read_reg(i2c_inst_t *i2c, uint8_t reg)
{
    uint8_t val = 0;
    i2c_write_blocking(i2c, SI5351_I2C_ADDR, &reg, 1, true);  // repeated start
    i2c_read_blocking(i2c, SI5351_I2C_ADDR, &val, 1, false);
    return val;
}

// Write 8 consecutive registers from an 8-byte array (used for PLL and MS blocks).
static void write_reg_block(i2c_inst_t *i2c, uint8_t base, const uint8_t *bytes)
{
    uint8_t buf[9];
    buf[0] = base;
    for (int i = 0; i < 8; i++) buf[i + 1] = bytes[i];
    i2c_write_blocking(i2c, SI5351_I2C_ADDR, buf, 9, false);
}

// ---------------------------------------------------------------------------
// Initialisation
// ---------------------------------------------------------------------------

bool si5351_init(i2c_inst_t *i2c)
{
    // Disable all outputs while we configure (reg 3: all bits set = all off).
    si5351_write_reg(i2c, SI5351_REG_OEB, 0xFF);

    // Set crystal load capacitance: 10 pF (bits 7:6 = 0b11, rest reserved as 0b010010).
    // AN619 table: reg 183 = 0xD2 for 10 pF.
    si5351_write_reg(i2c, SI5351_REG_XTAL_LOAD, 0xD2);

    // Wait for SYS_INIT (register 0 bit 7) to clear.
    // The Si5351a asserts this flag during its internal startup sequence.
    uint32_t timeout_ms = 100;
    while (timeout_ms--) {
        uint8_t status = si5351_read_reg(i2c, SI5351_REG_DEV_STATUS);
        if (!(status & 0x80)) break;   // SYS_INIT cleared: device is ready
        sleep_ms(1);
    }

    uint8_t status = si5351_read_reg(i2c, SI5351_REG_DEV_STATUS);
    return !(status & 0x80);   // return true if SYS_INIT cleared successfully
}

// ---------------------------------------------------------------------------
// Frequency calculation and programming
// ---------------------------------------------------------------------------

// Pack integer-mode multisynth parameters into the 8-register layout.
// AN619 §3.2 integer mode: P1 = 128*a - 512,  P2 = 0,  P3 = 1.
// Registers [BASE+0]...[BASE+7]:
//   [0]     P3[15:8]
//   [1]     P3[7:0]
//   [2]     R_DIV[2:0] | DIV_BY_4[1:0] | P1[17:16]
//   [3]     P1[15:8]
//   [4]     P1[7:0]
//   [5]     P3_MSB[3:0] | P2[19:16]
//   [6]     P2[15:8]
//   [7]     P2[7:0]
static void pack_ms_int(uint8_t out[8], uint32_t a)
{
    uint32_t P1 = 128 * a - 512;
    // P2 = 0, P3 = 1 for integer mode
    out[0] = 0;              // P3[15:8] = 0
    out[1] = 1;              // P3[7:0]  = 1
    out[2] = (P1 >> 16) & 0x03;  // R_DIV=0 (÷1), DIV_BY_4=0, P1[17:16]
    out[3] = (P1 >> 8) & 0xFF;
    out[4] = (P1 >> 0) & 0xFF;
    out[5] = 0;              // P3_MSB=0, P2[19:16]=0
    out[6] = 0;              // P2[15:8]=0
    out[7] = 0;              // P2[7:0]=0
}

// CLK control register value for integer mode, PLLA source, MS as output source.
// Bit 7 (PDN)=0, bit 6 (MS_INT)=1, bits 5:4 (MS_SRC=PLLA)=00,
// bit 3 (INV)=0, bits 2:0 (CLK_SRC=MS divider)=011
#define CLK_CTRL_INT_PLLA  0x4F

uint32_t si5351_set_freq_integer(i2c_inst_t *i2c, uint32_t freq_hz)
{
    // -----------------------------------------------------------------------
    // 1. Find the best (N, even M) pair.
    //    N: integer PLL multiplier, range 25-36 (gives VCO 614-885 MHz)
    //    M: integer MS divider, must be EVEN for 50% duty cycle (AN619 §4.1)
    //    Target: N * XTAL / M closest to freq_hz
    // -----------------------------------------------------------------------
    uint32_t best_N = 0, best_M = 0;
    uint32_t best_err = UINT32_MAX;

    for (uint32_t N = 25; N <= 36; N++) {
        uint64_t vco = (uint64_t)N * SI5351_XTAL_FREQ;
        if (vco < SI5351_VCO_MIN || vco > SI5351_VCO_MAX) continue;

        // M_ideal = vco / freq_hz; round down to nearest even
        uint32_t M_ideal = (uint32_t)(vco / freq_hz);
        if (M_ideal & 1) M_ideal--;   // force even

        // Check M-1 (even: M_ideal-2) and M+1 (even: M_ideal+2) as well
        for (int32_t delta = -2; delta <= 2; delta += 2) {
            uint32_t M = M_ideal + delta;
            if (M < 6 || M > 127) continue;   // Si5351a hard limits; M odd already excluded

            uint32_t f = (uint32_t)(vco / M);
            uint32_t err = (f > freq_hz) ? (f - freq_hz) : (freq_hz - f);
            if (err < best_err) {
                best_err = err;
                best_N   = N;
                best_M   = M;
            }
        }
    }

    if (best_N == 0) return 0;   // no valid pair found

    uint32_t actual_freq = (uint32_t)((uint64_t)best_N * SI5351_XTAL_FREQ / best_M);

    // -----------------------------------------------------------------------
    // 2. Program PLL A (MSNA) in integer mode.
    // -----------------------------------------------------------------------
    uint8_t pll_regs[8];
    pack_ms_int(pll_regs, best_N);
    write_reg_block(i2c, SI5351_REG_PLLA_BASE, pll_regs);

    // -----------------------------------------------------------------------
    // 3. Program MS0 (CLK0 / I channel) and MS1 (CLK1 / Q channel).
    //    Both use the same M; MS1 gets a 90 deg phase offset via PHOFF.
    // -----------------------------------------------------------------------
    uint8_t ms_regs[8];
    pack_ms_int(ms_regs, best_M);
    write_reg_block(i2c, SI5351_REG_MS0_BASE, ms_regs);
    write_reg_block(i2c, SI5351_REG_MS1_BASE, ms_regs);

    // -----------------------------------------------------------------------
    // 4. Configure CLK0 and CLK1 control registers.
    //    Both: integer mode, PLLA source, MS divider as output.
    // -----------------------------------------------------------------------
    si5351_write_reg(i2c, SI5351_REG_CLK0_CTRL, CLK_CTRL_INT_PLLA);
    si5351_write_reg(i2c, SI5351_REG_CLK1_CTRL, CLK_CTRL_INT_PLLA);

    // -----------------------------------------------------------------------
    // 5. Set quadrature phase offset.
    //    CLK0 (I) stays at 0 deg; CLK1 (Q) is delayed by PHOFF = M.
    //    PHOFF units: Tvco/4.  Delay = M * Tvco/4 = Tout/4 = exactly 90 deg.
    //    AN619 note: PHOFF register must be written BEFORE the PLL reset.
    // -----------------------------------------------------------------------
    si5351_write_reg(i2c, SI5351_REG_CLK0_PHOFF, 0);
    si5351_write_reg(i2c, SI5351_REG_CLK1_PHOFF, (uint8_t)best_M);

    // -----------------------------------------------------------------------
    // 6. Reset PLL A to latch the new parameters (bit 5 = PLLA reset).
    //    Write 0x20, NOT 0xA0 (which would also unnecessary reset PLLB).
    // -----------------------------------------------------------------------
    si5351_write_reg(i2c, SI5351_REG_PLL_RESET, 0x20);

    // -----------------------------------------------------------------------
    // 7. Enable CLK0 and CLK1 outputs (clear bits 0 and 1 of reg 3).
    //    CLK2 and higher remain disabled.
    // -----------------------------------------------------------------------
    si5351_write_reg(i2c, SI5351_REG_OEB, 0xFC);   // 1111 1100: CLK2-7 off, CLK0+CLK1 on

    return actual_freq;
}

void si5351_set_freq_regs(i2c_inst_t *i2c, uint32_t N,
                          uint32_t P1, uint32_t P2, uint32_t P3,
                          bool direct_mode)
{
    si5351_write_reg(i2c, SI5351_REG_CLK0_CTRL, CLK_CTRL_INT_PLLA);
    si5351_write_reg(i2c, SI5351_REG_CLK1_CTRL, CLK_CTRL_INT_PLLA);

    uint8_t ms_regs[8];
    pack_ms_int(ms_regs, N);
    write_reg_block(i2c, SI5351_REG_MS0_BASE, ms_regs);
    write_reg_block(i2c, SI5351_REG_MS1_BASE, ms_regs);

    uint8_t pll_regs[8];
    pll_regs[0] = (P3 >> 8) & 0xFF;
    pll_regs[1] =  P3       & 0xFF;
    pll_regs[2] = (P1 >> 16) & 0x03;
    pll_regs[3] = (P1 >>  8) & 0xFF;
    pll_regs[4] =  P1        & 0xFF;
    pll_regs[5] = ((P3 >> 12) & 0xF0) | ((P2 >> 16) & 0x0F);
    pll_regs[6] = (P2 >>  8) & 0xFF;
    pll_regs[7] =  P2        & 0xFF;
    write_reg_block(i2c, SI5351_REG_PLLA_BASE, pll_regs);

    si5351_write_reg(i2c, SI5351_REG_CLK0_PHOFF, 0);
    if (direct_mode) {
        si5351_write_reg(i2c, SI5351_REG_CLK1_PHOFF, (uint8_t)(N & 0x7F));
    }

    si5351_write_reg(i2c, SI5351_REG_PLL_RESET, 0xA0);
    si5351_write_reg(i2c, SI5351_REG_OEB, 0x00);
}
