#include "si5351.h"
#include "hardware/i2c.h"
#include "pico/stdlib.h"
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define CLK_CTRL_INT_PLLA  0x4F

static uint32_t gcd(uint32_t a, uint32_t b) {
    while (b != 0) {
        uint32_t t = b;
        b = a % b;
        a = t;
    }
    return a;
}

void si5351_write_reg(i2c_inst_t *i2c, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    i2c_write_blocking(i2c, SI5351_I2C_ADDR, buf, 2, false);
}

uint8_t si5351_read_reg(i2c_inst_t *i2c, uint8_t reg)
{
    uint8_t val = 0;
    i2c_write_blocking(i2c, SI5351_I2C_ADDR, &reg, 1, true);
    i2c_read_blocking(i2c, SI5351_I2C_ADDR, &val, 1, false);
    return val;
}

static void write_reg_block(i2c_inst_t *i2c, uint8_t base, const uint8_t *bytes)
{
    uint8_t buf[9];
    buf[0] = base;
    for (int i = 0; i < 8; i++) buf[i + 1] = bytes[i];
    i2c_write_blocking(i2c, SI5351_I2C_ADDR, buf, 9, false);
}

bool si5351_init(i2c_inst_t *i2c)
{
    si5351_write_reg(i2c, SI5351_REG_OEB, 0xFF);
    si5351_write_reg(i2c, SI5351_REG_XTAL_LOAD, 0xD2);
    // Output 24.576 MHz crystal clock on CLK2 for PCM1808 SCKI
    si5351_write_reg(i2c, SI5351_REG_CLK2_CTRL, 0x03);

    uint32_t timeout_ms = 100;
    while (timeout_ms--) {
        uint8_t status = si5351_read_reg(i2c, SI5351_REG_DEV_STATUS);
        if (!(status & 0x80)) break;
        sleep_ms(1);
    }

    si5351_write_reg(i2c, SI5351_REG_OEB, 0xF8);
    uint8_t status = si5351_read_reg(i2c, SI5351_REG_DEV_STATUS);
    return !(status & 0x80);
}

static void pack_ms_int(uint8_t out[8], uint32_t a)
{
    uint32_t P1 = 128 * a - 512;
    out[0] = 0;
    out[1] = 1;
    out[2] = (uint8_t)((P1 >> 16) & 0x03);
    out[3] = (uint8_t)((P1 >> 8) & 0xFF);
    out[4] = (uint8_t)((P1 >> 0) & 0xFF);
    out[5] = 0;
    out[6] = 0;
    out[7] = 0;
}

static void fill_candidate_regs(lo_candidate_t *c, uint32_t target_hz, uint32_t lo_hz,
                               uint32_t n, uint32_t a, uint32_t b, uint32_t c_val)
{
    c->target_hz = target_hz;
    c->actual_hz = lo_hz;
    c->offset_hz = (int32_t)lo_hz - (int32_t)target_hz;
    c->n = n;
    c->a = a;
    c->b = b;
    c->c = c_val;

    uint32_t floor_term = (128ULL * b) / c_val;
    c->p1 = 128 * a + floor_term - 512;
    c->p2 = 128 * b - c_val * floor_term;
    c->p3 = c_val;
    c->ptype = (b == 0) ? 'G' : 'F';
    c->valid = true;
}

typedef struct {
    int32_t rank_q;
    int32_t abs_offset;
    uint32_t n;
    uint32_t m;
    uint32_t lo_hz;
} integer_match_t;

static int compare_int_matches(const void *a, const void *b) {
    const integer_match_t *ia = (const integer_match_t *)a;
    const integer_match_t *ib = (const integer_match_t *)b;
    // Highest q first (weakest spur coupling)
    if (ia->rank_q != ib->rank_q) {
        return (ib->rank_q - ia->rank_q);
    }
    // Then closest offset
    if (ia->abs_offset != ib->abs_offset) {
        return (ia->abs_offset - ib->abs_offset);
    }
    // Then smallest N (lower phase noise)
    return (int)(ia->n - ib->n);
}

bool si5351_calculate_lo(uint32_t target_hz, uint32_t sample_rate, bool johnson_mode,
                         lo_calc_mode_t mode, lo_candidate_t *cand)
{
    memset(cand, 0, sizeof(*cand));
    uint32_t crystal = SI5351_XTAL_FREQ;
    uint32_t multiplier = johnson_mode ? 4 : 1;
    uint32_t n_step = johnson_mode ? 1 : 2;
    uint32_t half_bw = sample_rate / 2;

    uint64_t target_si5351 = (uint64_t)target_hz * multiplier;
    uint32_t n_min = (uint32_t)((SI5351_VCO_MIN + target_si5351 - 1) / target_si5351);
    if (n_min < (johnson_mode ? 4 : 6)) n_min = (johnson_mode ? 4 : 6);
    if (!johnson_mode && (n_min % 2 != 0)) n_min++;

    uint32_t n_max = (uint32_t)(SI5351_VCO_MAX / target_si5351);
    if (n_max > (johnson_mode ? 127 : 126)) n_max = (johnson_mode ? 127 : 126);

    if (mode != LO_MODE_FRACTIONAL_EXACT && n_min <= n_max) {
        integer_match_t matches[32];
        int count = 0;

        for (uint32_t n = n_min; n <= n_max && count < 32; n += n_step) {
            uint64_t prod = target_si5351 * n;
            uint32_t m = (uint32_t)((prod + (crystal / 2)) / crystal);
            if (m <= 14 || m >= 91) continue;

            uint64_t lo_si5351 = ((uint64_t)crystal * m) / n;
            uint32_t lo_logical = (uint32_t)(lo_si5351 / multiplier);
            int32_t off = (int32_t)lo_logical - (int32_t)target_hz;

            if (abs(off) < (int32_t)half_bw) {
                uint32_t g = gcd(m, n);
                matches[count].rank_q = (int32_t)(n / g);
                matches[count].abs_offset = abs(off);
                matches[count].n = n;
                matches[count].m = m;
                matches[count].lo_hz = lo_logical;
                count++;
            }
        }

        if (count > 0) {
            qsort(matches, count, sizeof(integer_match_t), compare_int_matches);
            int selected_idx = 0;
            if (mode == LO_MODE_MIDDLE_INTEGER) {
                selected_idx = count / 2;
            } else if (mode == LO_MODE_WORST_INTEGER) {
                selected_idx = count - 1;
            }
            fill_candidate_regs(cand, target_hz, matches[selected_idx].lo_hz,
                                matches[selected_idx].n, matches[selected_idx].m, 0, 1);
            return true;
        }
    }

    // Fractional fallback (exact frequency match)
    uint32_t n_val = n_min;
    if (n_val < (johnson_mode ? 4 : 6)) n_val = (johnson_mode ? 4 : 6);
    if (!johnson_mode && (n_val % 2 != 0)) n_val++;

    uint64_t num = target_si5351 * n_val;
    uint32_t a_val = (uint32_t)(num / crystal);
    uint64_t rem = num % crystal;

    // Reduce fraction rem / crystal to fit 20-bit P2/P3
    uint32_t c_val = 1000000;
    uint32_t b_val = (uint32_t)((rem * c_val) / crystal);
    uint32_t g = gcd(b_val, c_val);
    if (g > 1) {
        b_val /= g;
        c_val /= g;
    }
    if (c_val == 0) c_val = 1;

    fill_candidate_regs(cand, target_hz, target_hz, n_val, a_val, b_val, c_val);
    return true;
}

void si5351_apply_candidate(i2c_inst_t *i2c, const lo_candidate_t *cand, bool direct_mode)
{
    uint8_t ms_regs[8];
    pack_ms_int(ms_regs, cand->n);

    if (direct_mode) {
        si5351_write_reg(i2c, SI5351_REG_CLK0_CTRL, CLK_CTRL_INT_PLLA);
        si5351_write_reg(i2c, SI5351_REG_CLK1_CTRL, CLK_CTRL_INT_PLLA);
        write_reg_block(i2c, SI5351_REG_MS0_BASE, ms_regs);
        write_reg_block(i2c, SI5351_REG_MS1_BASE, ms_regs);
        si5351_write_reg(i2c, SI5351_REG_CLK0_PHOFF, 0);
        si5351_write_reg(i2c, SI5351_REG_CLK1_PHOFF, (uint8_t)(cand->n & 0x7F));
    } else {
        si5351_write_reg(i2c, SI5351_REG_CLK1_CTRL, CLK_CTRL_INT_PLLA);
        write_reg_block(i2c, SI5351_REG_MS1_BASE, ms_regs);
        si5351_write_reg(i2c, SI5351_REG_CLK1_PHOFF, 0);
    }

    uint8_t pll_regs[8];
    pll_regs[0] = (cand->p3 >> 8) & 0xFF;
    pll_regs[1] =  cand->p3       & 0xFF;
    pll_regs[2] = (cand->p1 >> 16) & 0x03;
    pll_regs[3] = (cand->p1 >>  8) & 0xFF;
    pll_regs[4] =  cand->p1        & 0xFF;
    pll_regs[5] = ((cand->p3 >> 12) & 0xF0) | ((cand->p2 >> 16) & 0x0F);
    pll_regs[6] = (cand->p2 >>  8) & 0xFF;
    pll_regs[7] =  cand->p2        & 0xFF;
    write_reg_block(i2c, SI5351_REG_PLLA_BASE, pll_regs);

    si5351_write_reg(i2c, SI5351_REG_PLL_RESET, 0x20);
    si5351_write_reg(i2c, SI5351_REG_OEB, 0xF8);
}

void si5351_set_freq_regs(i2c_inst_t *i2c, uint32_t N,
                          uint32_t P1, uint32_t P2, uint32_t P3,
                          bool direct_mode)
{
    lo_candidate_t cand;
    memset(&cand, 0, sizeof(cand));
    cand.n = N;
    cand.p1 = P1;
    cand.p2 = P2;
    cand.p3 = P3;
    si5351_apply_candidate(i2c, &cand, direct_mode);
}

bool si5351_tune_frequency(i2c_inst_t *i2c, uint32_t target_hz, uint32_t sample_rate,
                           bool direct_mode, lo_calc_mode_t mode, lo_candidate_t *applied_cand)
{
    lo_candidate_t cand;
    bool ok = si5351_calculate_lo(target_hz, sample_rate, !direct_mode, mode, &cand);
    if (!ok) return false;

    si5351_apply_candidate(i2c, &cand, direct_mode);
    if (applied_cand) {
        *applied_cand = cand;
    }
    return true;
}
