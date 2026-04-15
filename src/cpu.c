/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cpu.h"
#include <string.h>

/* ============================================================
 *   Cycle-counting bus wrappers
 * ============================================================ */

static uint8_t r8(struct cpu *c, uint16_t a)
{
    uint8_t v = c->bus.read(c->bus.ctx, a);
    c->cycles++;
    return v;
}

static void w8(struct cpu *c, uint16_t a, uint8_t v)
{
    c->bus.write(c->bus.ctx, a, v);
    c->cycles++;
}

/* ============================================================
 *   Flag helpers
 * ============================================================ */

static void snz(struct cpu *c, uint8_t v)
{
    c->p = (uint8_t)((c->p & ~(CPU_FLAG_N | CPU_FLAG_Z))
                     | (v & CPU_FLAG_N)
                     | (v == 0 ? CPU_FLAG_Z : 0));
}

static void set_flag(struct cpu *c, uint8_t mask, int set)
{
    if (set) c->p |= mask;
    else     c->p &= (uint8_t)~mask;
}

/* ============================================================
 *   Stack
 * ============================================================ */

static void push(struct cpu *c, uint8_t v)
{
    w8(c, (uint16_t)(0x0100 | c->s), v);
    c->s--;
}

static uint8_t pull(struct cpu *c)
{
    c->s++;
    return r8(c, (uint16_t)(0x0100 | c->s));
}

/* ============================================================
 *   Addressing modes — effective-address + cycle-correct fetches
 *
 *   _r variants add a page-cross dummy read only on page cross.
 *   _rw variants always add the dummy read (writes and RMW).
 * ============================================================ */

static uint16_t ea_zp(struct cpu *c)
{
    return r8(c, c->pc++);
}

static uint16_t ea_zp_x(struct cpu *c)
{
    uint8_t a = r8(c, c->pc++);
    r8(c, a);
    return (uint8_t)(a + c->x);
}

static uint16_t ea_zp_y(struct cpu *c)
{
    uint8_t a = r8(c, c->pc++);
    r8(c, a);
    return (uint8_t)(a + c->y);
}

static uint16_t ea_abs(struct cpu *c)
{
    uint8_t lo = r8(c, c->pc++);
    uint8_t hi = r8(c, c->pc++);
    return (uint16_t)((uint16_t)lo | ((uint16_t)hi << 8));
}

static uint16_t ea_abs_x_r(struct cpu *c)
{
    uint8_t lo = r8(c, c->pc++);
    uint8_t hi = r8(c, c->pc++);
    uint16_t base = (uint16_t)((uint16_t)lo | ((uint16_t)hi << 8));
    uint16_t eff  = (uint16_t)(base + c->x);
    if ((eff & 0xFF00) != (base & 0xFF00))
        r8(c, (uint16_t)(((uint16_t)hi << 8) | (uint8_t)(lo + c->x)));
    return eff;
}

static uint16_t ea_abs_x_rw(struct cpu *c)
{
    uint8_t lo = r8(c, c->pc++);
    uint8_t hi = r8(c, c->pc++);
    uint16_t base = (uint16_t)((uint16_t)lo | ((uint16_t)hi << 8));
    r8(c, (uint16_t)(((uint16_t)hi << 8) | (uint8_t)(lo + c->x)));
    return (uint16_t)(base + c->x);
}

static uint16_t ea_abs_y_r(struct cpu *c)
{
    uint8_t lo = r8(c, c->pc++);
    uint8_t hi = r8(c, c->pc++);
    uint16_t base = (uint16_t)((uint16_t)lo | ((uint16_t)hi << 8));
    uint16_t eff  = (uint16_t)(base + c->y);
    if ((eff & 0xFF00) != (base & 0xFF00))
        r8(c, (uint16_t)(((uint16_t)hi << 8) | (uint8_t)(lo + c->y)));
    return eff;
}

static uint16_t ea_abs_y_rw(struct cpu *c)
{
    uint8_t lo = r8(c, c->pc++);
    uint8_t hi = r8(c, c->pc++);
    uint16_t base = (uint16_t)((uint16_t)lo | ((uint16_t)hi << 8));
    r8(c, (uint16_t)(((uint16_t)hi << 8) | (uint8_t)(lo + c->y)));
    return (uint16_t)(base + c->y);
}

static uint16_t ea_izx(struct cpu *c)
{
    uint8_t zp = r8(c, c->pc++);
    uint8_t p;
    uint8_t lo, hi;
    r8(c, zp);
    p  = (uint8_t)(zp + c->x);
    lo = r8(c, p);
    hi = r8(c, (uint8_t)(p + 1));
    return (uint16_t)((uint16_t)lo | ((uint16_t)hi << 8));
}

static uint16_t ea_izy_r(struct cpu *c)
{
    uint8_t zp = r8(c, c->pc++);
    uint8_t lo = r8(c, zp);
    uint8_t hi = r8(c, (uint8_t)(zp + 1));
    uint16_t base = (uint16_t)((uint16_t)lo | ((uint16_t)hi << 8));
    uint16_t eff  = (uint16_t)(base + c->y);
    if ((eff & 0xFF00) != (base & 0xFF00))
        r8(c, (uint16_t)(((uint16_t)hi << 8) | (uint8_t)(lo + c->y)));
    return eff;
}

static uint16_t ea_izy_rw(struct cpu *c)
{
    uint8_t zp = r8(c, c->pc++);
    uint8_t lo = r8(c, zp);
    uint8_t hi = r8(c, (uint8_t)(zp + 1));
    uint16_t base = (uint16_t)((uint16_t)lo | ((uint16_t)hi << 8));
    r8(c, (uint16_t)(((uint16_t)hi << 8) | (uint8_t)(lo + c->y)));
    return (uint16_t)(base + c->y);
}

/* ============================================================
 *   ALU helpers
 * ============================================================ */

static void do_adc(struct cpu *c, uint8_t m)
{
    uint8_t a = c->a;
    uint8_t cin = (c->p & CPU_FLAG_C) ? 1u : 0u;
    if (c->p & CPU_FLAG_D) {
        /* NMOS BCD ADC per Bruce Clark. */
        unsigned bin = (unsigned)a + (unsigned)m + cin;
        unsigned al  = (unsigned)(a & 0x0F) + (unsigned)(m & 0x0F) + cin;
        unsigned ah;
        if (al > 9) al = ((al + 6) & 0x0F) + 0x10;
        ah = (unsigned)(a & 0xF0) + (unsigned)(m & 0xF0) + al;
        set_flag(c, CPU_FLAG_Z, (bin & 0xFF) == 0);
        set_flag(c, CPU_FLAG_N, (ah & 0x80) != 0);
        set_flag(c, CPU_FLAG_V, (~(a ^ m) & (a ^ ah) & 0x80) != 0);
        if (ah >= 0xA0) ah += 0x60;
        set_flag(c, CPU_FLAG_C, ah >= 0x100);
        c->a = (uint8_t)(ah & 0xFF);
    } else {
        unsigned sum = (unsigned)a + (unsigned)m + cin;
        uint8_t r = (uint8_t)sum;
        set_flag(c, CPU_FLAG_C, sum > 0xFF);
        set_flag(c, CPU_FLAG_V, ((a ^ r) & (m ^ r) & 0x80) != 0);
        snz(c, r);
        c->a = r;
    }
}

static void do_sbc(struct cpu *c, uint8_t m)
{
    uint8_t a = c->a;
    uint8_t cin = (c->p & CPU_FLAG_C) ? 1u : 0u;
    uint8_t mn = (uint8_t)~m;
    unsigned bin = (unsigned)a + (unsigned)mn + cin;
    uint8_t r = (uint8_t)bin;
    /* Binary flags on NMOS regardless of D. */
    set_flag(c, CPU_FLAG_C, bin > 0xFF);
    set_flag(c, CPU_FLAG_V, ((a ^ r) & (mn ^ r) & 0x80) != 0);
    snz(c, r);
    if (c->p & CPU_FLAG_D) {
        int al = (a & 0x0F) - (m & 0x0F) - (cin ? 0 : 1);
        int ah = (a >> 4)   - (m >> 4)   - (al < 0 ? 1 : 0);
        if (al < 0) al -= 6;
        if (ah < 0) ah -= 6;
        c->a = (uint8_t)(((ah & 0x0F) << 4) | (al & 0x0F));
    } else {
        c->a = r;
    }
}

static void do_cmp(struct cpu *c, uint8_t r, uint8_t m)
{
    uint8_t t = (uint8_t)(r - m);
    set_flag(c, CPU_FLAG_C, r >= m);
    snz(c, t);
}

static void do_bit(struct cpu *c, uint8_t m)
{
    set_flag(c, CPU_FLAG_Z, (c->a & m) == 0);
    set_flag(c, CPU_FLAG_N, (m & 0x80) != 0);
    set_flag(c, CPU_FLAG_V, (m & 0x40) != 0);
}

static uint8_t do_asl(struct cpu *c, uint8_t v)
{
    set_flag(c, CPU_FLAG_C, (v & 0x80) != 0);
    v = (uint8_t)(v << 1);
    snz(c, v);
    return v;
}

static uint8_t do_lsr(struct cpu *c, uint8_t v)
{
    set_flag(c, CPU_FLAG_C, (v & 0x01) != 0);
    v = (uint8_t)(v >> 1);
    snz(c, v);
    return v;
}

static uint8_t do_rol(struct cpu *c, uint8_t v)
{
    uint8_t r = (uint8_t)((v << 1) | ((c->p & CPU_FLAG_C) ? 0x01 : 0x00));
    set_flag(c, CPU_FLAG_C, (v & 0x80) != 0);
    snz(c, r);
    return r;
}

static uint8_t do_ror(struct cpu *c, uint8_t v)
{
    uint8_t r = (uint8_t)((v >> 1) | ((c->p & CPU_FLAG_C) ? 0x80 : 0x00));
    set_flag(c, CPU_FLAG_C, (v & 0x01) != 0);
    snz(c, r);
    return r;
}

static void do_branch(struct cpu *c, int taken)
{
    int8_t off = (int8_t)r8(c, c->pc++);
    if (taken) {
        uint16_t old_pc = c->pc;
        uint16_t new_pc = (uint16_t)(old_pc + off);
        r8(c, old_pc);
        if ((new_pc & 0xFF00) != (old_pc & 0xFF00))
            r8(c, (uint16_t)((old_pc & 0xFF00) | (new_pc & 0x00FF)));
        c->pc = new_pc;
    }
}

/* Read-Modify-Write at a given effective address.
 * The 6502 does: read -> dummy-write(original) -> write(modified). */
static void do_rmw(struct cpu *c, uint16_t ea, uint8_t (*op)(struct cpu *, uint8_t))
{
    uint8_t m = r8(c, ea);
    w8(c, ea, m);
    m = op(c, m);
    w8(c, ea, m);
}

/* ============================================================
 *   Opcode handlers — 256 entries, named op_NN by opcode byte
 * ============================================================ */

/* 00: BRK */
static void op_00(struct cpu *c)
{
    r8(c, c->pc++);                                     /* pad byte */
    push(c, (uint8_t)(c->pc >> 8));
    push(c, (uint8_t)(c->pc & 0xFF));
    push(c, (uint8_t)(c->p | CPU_FLAG_B | CPU_FLAG_U));
    c->p |= CPU_FLAG_I;
    {
        uint8_t lo = r8(c, 0xFFFE);
        uint8_t hi = r8(c, 0xFFFF);
        c->pc = (uint16_t)((uint16_t)lo | ((uint16_t)hi << 8));
    }
}

/* 01: ORA (zp,X) */
static void op_01(struct cpu *c) { uint16_t ea = ea_izx(c); c->a |= r8(c, ea); snz(c, c->a); }

/* JAM (a.k.a. KIL, HLT, CRS): 12 illegal NMOS opcodes that lock the CPU.
 * After the opcode fetch, the CPU's microcode gets stuck in a fixed
 * read-only pattern that drives the address bus through PC+1 and the
 * IRQ/NMI-vector region ($FFFE/$FFFF) before settling. PC is only
 * incremented once (for the opcode fetch). The cycle pattern (11 total
 * reads, same for every JAM variant) is what the Harte test vectors
 * model. Registers/flags unchanged, halted. */
static void op_jam(struct cpu *c)
{
    r8(c, c->pc);
    r8(c, 0xFFFF);
    r8(c, 0xFFFE);
    r8(c, 0xFFFE);
    r8(c, 0xFFFF);
    r8(c, 0xFFFF);
    r8(c, 0xFFFF);
    r8(c, 0xFFFF);
    r8(c, 0xFFFF);
    r8(c, 0xFFFF);
    c->halted = true;
}

/* 03: SLO (zp,X) */
static void op_03(struct cpu *c)
{
    uint16_t ea = ea_izx(c);
    uint8_t m = r8(c, ea);
    w8(c, ea, m);
    m = do_asl(c, m);
    w8(c, ea, m);
    c->a |= m; snz(c, c->a);
}

/* 04: NOP zp */
static void op_04(struct cpu *c) { uint16_t ea = ea_zp(c); r8(c, ea); }

/* 05: ORA zp */
static void op_05(struct cpu *c) { uint16_t ea = ea_zp(c); c->a |= r8(c, ea); snz(c, c->a); }

/* 06: ASL zp */
static void op_06(struct cpu *c) { do_rmw(c, ea_zp(c), do_asl); }

/* 07: SLO zp */
static void op_07(struct cpu *c)
{
    uint16_t ea = ea_zp(c);
    uint8_t m = r8(c, ea);
    w8(c, ea, m);
    m = do_asl(c, m);
    w8(c, ea, m);
    c->a |= m; snz(c, c->a);
}

/* 08: PHP */
static void op_08(struct cpu *c) { r8(c, c->pc); push(c, (uint8_t)(c->p | CPU_FLAG_B | CPU_FLAG_U)); }

/* 09: ORA #imm */
static void op_09(struct cpu *c) { c->a |= r8(c, c->pc++); snz(c, c->a); }

/* 0A: ASL A */
static void op_0A(struct cpu *c) { r8(c, c->pc); c->a = do_asl(c, c->a); }

/* 0B: ANC #imm (undoc) */
static void op_0B(struct cpu *c)
{
    c->a &= r8(c, c->pc++);
    snz(c, c->a);
    set_flag(c, CPU_FLAG_C, (c->a & 0x80) != 0);
}

/* 0C: NOP abs (undoc) */
static void op_0C(struct cpu *c) { uint16_t ea = ea_abs(c); r8(c, ea); }

/* 0D: ORA abs */
static void op_0D(struct cpu *c) { uint16_t ea = ea_abs(c); c->a |= r8(c, ea); snz(c, c->a); }

/* 0E: ASL abs */
static void op_0E(struct cpu *c) { do_rmw(c, ea_abs(c), do_asl); }

/* 0F: SLO abs (undoc) */
static void op_0F(struct cpu *c)
{
    uint16_t ea = ea_abs(c);
    uint8_t m = r8(c, ea);
    w8(c, ea, m);
    m = do_asl(c, m);
    w8(c, ea, m);
    c->a |= m; snz(c, c->a);
}

/* 10: BPL */
static void op_10(struct cpu *c) { do_branch(c, !(c->p & CPU_FLAG_N)); }

/* 11: ORA (zp),Y */
static void op_11(struct cpu *c) { uint16_t ea = ea_izy_r(c); c->a |= r8(c, ea); snz(c, c->a); }

/* 13: SLO (zp),Y */
static void op_13(struct cpu *c)
{
    uint16_t ea = ea_izy_rw(c);
    uint8_t m = r8(c, ea);
    w8(c, ea, m);
    m = do_asl(c, m);
    w8(c, ea, m);
    c->a |= m; snz(c, c->a);
}

/* 14: NOP zp,X */
static void op_14(struct cpu *c) { uint16_t ea = ea_zp_x(c); r8(c, ea); }

/* 15: ORA zp,X */
static void op_15(struct cpu *c) { uint16_t ea = ea_zp_x(c); c->a |= r8(c, ea); snz(c, c->a); }

/* 16: ASL zp,X */
static void op_16(struct cpu *c) { do_rmw(c, ea_zp_x(c), do_asl); }

/* 17: SLO zp,X */
static void op_17(struct cpu *c)
{
    uint16_t ea = ea_zp_x(c);
    uint8_t m = r8(c, ea);
    w8(c, ea, m);
    m = do_asl(c, m);
    w8(c, ea, m);
    c->a |= m; snz(c, c->a);
}

/* 18: CLC */
static void op_18(struct cpu *c) { r8(c, c->pc); c->p &= (uint8_t)~CPU_FLAG_C; }

/* 19: ORA abs,Y */
static void op_19(struct cpu *c) { uint16_t ea = ea_abs_y_r(c); c->a |= r8(c, ea); snz(c, c->a); }

/* 1A: NOP (undoc) */
static void op_1A(struct cpu *c) { r8(c, c->pc); }

/* 1B: SLO abs,Y */
static void op_1B(struct cpu *c)
{
    uint16_t ea = ea_abs_y_rw(c);
    uint8_t m = r8(c, ea);
    w8(c, ea, m);
    m = do_asl(c, m);
    w8(c, ea, m);
    c->a |= m; snz(c, c->a);
}

/* 1C: NOP abs,X */
static void op_1C(struct cpu *c) { uint16_t ea = ea_abs_x_r(c); r8(c, ea); }

/* 1D: ORA abs,X */
static void op_1D(struct cpu *c) { uint16_t ea = ea_abs_x_r(c); c->a |= r8(c, ea); snz(c, c->a); }

/* 1E: ASL abs,X */
static void op_1E(struct cpu *c) { do_rmw(c, ea_abs_x_rw(c), do_asl); }

/* 1F: SLO abs,X */
static void op_1F(struct cpu *c)
{
    uint16_t ea = ea_abs_x_rw(c);
    uint8_t m = r8(c, ea);
    w8(c, ea, m);
    m = do_asl(c, m);
    w8(c, ea, m);
    c->a |= m; snz(c, c->a);
}

/* 20: JSR abs */
static void op_20(struct cpu *c)
{
    uint8_t lo = r8(c, c->pc++);
    r8(c, (uint16_t)(0x0100 | c->s));                   /* internal */
    push(c, (uint8_t)(c->pc >> 8));
    push(c, (uint8_t)(c->pc & 0xFF));
    {
        uint8_t hi = r8(c, c->pc);
        c->pc = (uint16_t)((uint16_t)lo | ((uint16_t)hi << 8));
    }
}

/* 21: AND (zp,X) */
static void op_21(struct cpu *c) { uint16_t ea = ea_izx(c); c->a &= r8(c, ea); snz(c, c->a); }

/* 23: RLA (zp,X) */
static void op_23(struct cpu *c)
{
    uint16_t ea = ea_izx(c);
    uint8_t m = r8(c, ea);
    w8(c, ea, m);
    m = do_rol(c, m);
    w8(c, ea, m);
    c->a &= m; snz(c, c->a);
}

/* 24: BIT zp */
static void op_24(struct cpu *c) { uint16_t ea = ea_zp(c); do_bit(c, r8(c, ea)); }

/* 25: AND zp */
static void op_25(struct cpu *c) { uint16_t ea = ea_zp(c); c->a &= r8(c, ea); snz(c, c->a); }

/* 26: ROL zp */
static void op_26(struct cpu *c) { do_rmw(c, ea_zp(c), do_rol); }

/* 27: RLA zp */
static void op_27(struct cpu *c)
{
    uint16_t ea = ea_zp(c);
    uint8_t m = r8(c, ea);
    w8(c, ea, m);
    m = do_rol(c, m);
    w8(c, ea, m);
    c->a &= m; snz(c, c->a);
}

/* 28: PLP */
static void op_28(struct cpu *c)
{
    r8(c, c->pc);
    r8(c, (uint16_t)(0x0100 | c->s));
    c->p = (uint8_t)((pull(c) & ~CPU_FLAG_B) | CPU_FLAG_U);
}

/* 29: AND #imm */
static void op_29(struct cpu *c) { c->a &= r8(c, c->pc++); snz(c, c->a); }

/* 2A: ROL A */
static void op_2A(struct cpu *c) { r8(c, c->pc); c->a = do_rol(c, c->a); }

/* 2B: ANC #imm (undoc, same as 0B) */
static void op_2B(struct cpu *c) { op_0B(c); }

/* 2C: BIT abs */
static void op_2C(struct cpu *c) { uint16_t ea = ea_abs(c); do_bit(c, r8(c, ea)); }

/* 2D: AND abs */
static void op_2D(struct cpu *c) { uint16_t ea = ea_abs(c); c->a &= r8(c, ea); snz(c, c->a); }

/* 2E: ROL abs */
static void op_2E(struct cpu *c) { do_rmw(c, ea_abs(c), do_rol); }

/* 2F: RLA abs */
static void op_2F(struct cpu *c)
{
    uint16_t ea = ea_abs(c);
    uint8_t m = r8(c, ea);
    w8(c, ea, m);
    m = do_rol(c, m);
    w8(c, ea, m);
    c->a &= m; snz(c, c->a);
}

/* 30: BMI */
static void op_30(struct cpu *c) { do_branch(c, (c->p & CPU_FLAG_N) != 0); }

/* 31: AND (zp),Y */
static void op_31(struct cpu *c) { uint16_t ea = ea_izy_r(c); c->a &= r8(c, ea); snz(c, c->a); }

/* 33: RLA (zp),Y */
static void op_33(struct cpu *c)
{
    uint16_t ea = ea_izy_rw(c);
    uint8_t m = r8(c, ea);
    w8(c, ea, m);
    m = do_rol(c, m);
    w8(c, ea, m);
    c->a &= m; snz(c, c->a);
}

/* 34: NOP zp,X */
static void op_34(struct cpu *c) { uint16_t ea = ea_zp_x(c); r8(c, ea); }

/* 35: AND zp,X */
static void op_35(struct cpu *c) { uint16_t ea = ea_zp_x(c); c->a &= r8(c, ea); snz(c, c->a); }

/* 36: ROL zp,X */
static void op_36(struct cpu *c) { do_rmw(c, ea_zp_x(c), do_rol); }

/* 37: RLA zp,X */
static void op_37(struct cpu *c)
{
    uint16_t ea = ea_zp_x(c);
    uint8_t m = r8(c, ea);
    w8(c, ea, m);
    m = do_rol(c, m);
    w8(c, ea, m);
    c->a &= m; snz(c, c->a);
}

/* 38: SEC */
static void op_38(struct cpu *c) { r8(c, c->pc); c->p |= CPU_FLAG_C; }

/* 39: AND abs,Y */
static void op_39(struct cpu *c) { uint16_t ea = ea_abs_y_r(c); c->a &= r8(c, ea); snz(c, c->a); }

/* 3A: NOP */
static void op_3A(struct cpu *c) { r8(c, c->pc); }

/* 3B: RLA abs,Y */
static void op_3B(struct cpu *c)
{
    uint16_t ea = ea_abs_y_rw(c);
    uint8_t m = r8(c, ea);
    w8(c, ea, m);
    m = do_rol(c, m);
    w8(c, ea, m);
    c->a &= m; snz(c, c->a);
}

/* 3C: NOP abs,X */
static void op_3C(struct cpu *c) { uint16_t ea = ea_abs_x_r(c); r8(c, ea); }

/* 3D: AND abs,X */
static void op_3D(struct cpu *c) { uint16_t ea = ea_abs_x_r(c); c->a &= r8(c, ea); snz(c, c->a); }

/* 3E: ROL abs,X */
static void op_3E(struct cpu *c) { do_rmw(c, ea_abs_x_rw(c), do_rol); }

/* 3F: RLA abs,X */
static void op_3F(struct cpu *c)
{
    uint16_t ea = ea_abs_x_rw(c);
    uint8_t m = r8(c, ea);
    w8(c, ea, m);
    m = do_rol(c, m);
    w8(c, ea, m);
    c->a &= m; snz(c, c->a);
}

/* 40: RTI */
static void op_40(struct cpu *c)
{
    uint8_t lo, hi;
    r8(c, c->pc);
    r8(c, (uint16_t)(0x0100 | c->s));
    c->p = (uint8_t)((pull(c) & ~CPU_FLAG_B) | CPU_FLAG_U);
    lo = pull(c);
    hi = pull(c);
    c->pc = (uint16_t)((uint16_t)lo | ((uint16_t)hi << 8));
}

/* 41: EOR (zp,X) */
static void op_41(struct cpu *c) { uint16_t ea = ea_izx(c); c->a ^= r8(c, ea); snz(c, c->a); }

/* 43: SRE (zp,X) */
static void op_43(struct cpu *c)
{
    uint16_t ea = ea_izx(c);
    uint8_t m = r8(c, ea);
    w8(c, ea, m);
    m = do_lsr(c, m);
    w8(c, ea, m);
    c->a ^= m; snz(c, c->a);
}

/* 44: NOP zp */
static void op_44(struct cpu *c) { uint16_t ea = ea_zp(c); r8(c, ea); }

/* 45: EOR zp */
static void op_45(struct cpu *c) { uint16_t ea = ea_zp(c); c->a ^= r8(c, ea); snz(c, c->a); }

/* 46: LSR zp */
static void op_46(struct cpu *c) { do_rmw(c, ea_zp(c), do_lsr); }

/* 47: SRE zp */
static void op_47(struct cpu *c)
{
    uint16_t ea = ea_zp(c);
    uint8_t m = r8(c, ea);
    w8(c, ea, m);
    m = do_lsr(c, m);
    w8(c, ea, m);
    c->a ^= m; snz(c, c->a);
}

/* 48: PHA */
static void op_48(struct cpu *c) { r8(c, c->pc); push(c, c->a); }

/* 49: EOR #imm */
static void op_49(struct cpu *c) { c->a ^= r8(c, c->pc++); snz(c, c->a); }

/* 4A: LSR A */
static void op_4A(struct cpu *c) { r8(c, c->pc); c->a = do_lsr(c, c->a); }

/* 4B: ALR #imm (undoc) */
static void op_4B(struct cpu *c)
{
    c->a &= r8(c, c->pc++);
    c->a = do_lsr(c, c->a);
}

/* 4C: JMP abs */
static void op_4C(struct cpu *c) { c->pc = ea_abs(c); }

/* 4D: EOR abs */
static void op_4D(struct cpu *c) { uint16_t ea = ea_abs(c); c->a ^= r8(c, ea); snz(c, c->a); }

/* 4E: LSR abs */
static void op_4E(struct cpu *c) { do_rmw(c, ea_abs(c), do_lsr); }

/* 4F: SRE abs */
static void op_4F(struct cpu *c)
{
    uint16_t ea = ea_abs(c);
    uint8_t m = r8(c, ea);
    w8(c, ea, m);
    m = do_lsr(c, m);
    w8(c, ea, m);
    c->a ^= m; snz(c, c->a);
}

/* 50: BVC */
static void op_50(struct cpu *c) { do_branch(c, !(c->p & CPU_FLAG_V)); }

/* 51: EOR (zp),Y */
static void op_51(struct cpu *c) { uint16_t ea = ea_izy_r(c); c->a ^= r8(c, ea); snz(c, c->a); }

/* 53: SRE (zp),Y */
static void op_53(struct cpu *c)
{
    uint16_t ea = ea_izy_rw(c);
    uint8_t m = r8(c, ea);
    w8(c, ea, m);
    m = do_lsr(c, m);
    w8(c, ea, m);
    c->a ^= m; snz(c, c->a);
}

/* 54: NOP zp,X */
static void op_54(struct cpu *c) { uint16_t ea = ea_zp_x(c); r8(c, ea); }

/* 55: EOR zp,X */
static void op_55(struct cpu *c) { uint16_t ea = ea_zp_x(c); c->a ^= r8(c, ea); snz(c, c->a); }

/* 56: LSR zp,X */
static void op_56(struct cpu *c) { do_rmw(c, ea_zp_x(c), do_lsr); }

/* 57: SRE zp,X */
static void op_57(struct cpu *c)
{
    uint16_t ea = ea_zp_x(c);
    uint8_t m = r8(c, ea);
    w8(c, ea, m);
    m = do_lsr(c, m);
    w8(c, ea, m);
    c->a ^= m; snz(c, c->a);
}

/* 58: CLI */
static void op_58(struct cpu *c) { r8(c, c->pc); c->p &= (uint8_t)~CPU_FLAG_I; }

/* 59: EOR abs,Y */
static void op_59(struct cpu *c) { uint16_t ea = ea_abs_y_r(c); c->a ^= r8(c, ea); snz(c, c->a); }

/* 5A: NOP */
static void op_5A(struct cpu *c) { r8(c, c->pc); }

/* 5B: SRE abs,Y */
static void op_5B(struct cpu *c)
{
    uint16_t ea = ea_abs_y_rw(c);
    uint8_t m = r8(c, ea);
    w8(c, ea, m);
    m = do_lsr(c, m);
    w8(c, ea, m);
    c->a ^= m; snz(c, c->a);
}

/* 5C: NOP abs,X */
static void op_5C(struct cpu *c) { uint16_t ea = ea_abs_x_r(c); r8(c, ea); }

/* 5D: EOR abs,X */
static void op_5D(struct cpu *c) { uint16_t ea = ea_abs_x_r(c); c->a ^= r8(c, ea); snz(c, c->a); }

/* 5E: LSR abs,X */
static void op_5E(struct cpu *c) { do_rmw(c, ea_abs_x_rw(c), do_lsr); }

/* 5F: SRE abs,X */
static void op_5F(struct cpu *c)
{
    uint16_t ea = ea_abs_x_rw(c);
    uint8_t m = r8(c, ea);
    w8(c, ea, m);
    m = do_lsr(c, m);
    w8(c, ea, m);
    c->a ^= m; snz(c, c->a);
}

/* 60: RTS */
static void op_60(struct cpu *c)
{
    uint8_t lo, hi;
    r8(c, c->pc);
    r8(c, (uint16_t)(0x0100 | c->s));
    lo = pull(c);
    hi = pull(c);
    c->pc = (uint16_t)((uint16_t)lo | ((uint16_t)hi << 8));
    r8(c, c->pc);
    c->pc++;
}

/* 61: ADC (zp,X) */
static void op_61(struct cpu *c) { uint16_t ea = ea_izx(c); do_adc(c, r8(c, ea)); }

/* 63: RRA (zp,X) */
static void op_63(struct cpu *c)
{
    uint16_t ea = ea_izx(c);
    uint8_t m = r8(c, ea);
    w8(c, ea, m);
    m = do_ror(c, m);
    w8(c, ea, m);
    do_adc(c, m);
}

/* 64: NOP zp */
static void op_64(struct cpu *c) { uint16_t ea = ea_zp(c); r8(c, ea); }

/* 65: ADC zp */
static void op_65(struct cpu *c) { uint16_t ea = ea_zp(c); do_adc(c, r8(c, ea)); }

/* 66: ROR zp */
static void op_66(struct cpu *c) { do_rmw(c, ea_zp(c), do_ror); }

/* 67: RRA zp */
static void op_67(struct cpu *c)
{
    uint16_t ea = ea_zp(c);
    uint8_t m = r8(c, ea);
    w8(c, ea, m);
    m = do_ror(c, m);
    w8(c, ea, m);
    do_adc(c, m);
}

/* 68: PLA */
static void op_68(struct cpu *c)
{
    r8(c, c->pc);
    r8(c, (uint16_t)(0x0100 | c->s));
    c->a = pull(c);
    snz(c, c->a);
}

/* 69: ADC #imm */
static void op_69(struct cpu *c) { do_adc(c, r8(c, c->pc++)); }

/* 6A: ROR A */
static void op_6A(struct cpu *c) { r8(c, c->pc); c->a = do_ror(c, c->a); }

/* 6B: ARR #imm (undoc) */
static void op_6B(struct cpu *c)
{
    uint8_t m = r8(c, c->pc++);
    uint8_t t = (uint8_t)(c->a & m);
    c->a = (uint8_t)((t >> 1) | ((c->p & CPU_FLAG_C) ? 0x80 : 0x00));
    snz(c, c->a);
    set_flag(c, CPU_FLAG_C, (c->a & 0x40) != 0);
    set_flag(c, CPU_FLAG_V, (((c->a >> 6) ^ (c->a >> 5)) & 0x01) != 0);
}

/* 6C: JMP (abs) — with the page-boundary bug */
static void op_6C(struct cpu *c)
{
    uint8_t lo = r8(c, c->pc++);
    uint8_t hi = r8(c, c->pc++);
    uint16_t ptr = (uint16_t)((uint16_t)lo | ((uint16_t)hi << 8));
    uint8_t tlo = r8(c, ptr);
    uint8_t thi = r8(c, (uint16_t)(((uint16_t)hi << 8) | (uint8_t)(lo + 1)));
    c->pc = (uint16_t)((uint16_t)tlo | ((uint16_t)thi << 8));
}

/* 6D: ADC abs */
static void op_6D(struct cpu *c) { uint16_t ea = ea_abs(c); do_adc(c, r8(c, ea)); }

/* 6E: ROR abs */
static void op_6E(struct cpu *c) { do_rmw(c, ea_abs(c), do_ror); }

/* 6F: RRA abs */
static void op_6F(struct cpu *c)
{
    uint16_t ea = ea_abs(c);
    uint8_t m = r8(c, ea);
    w8(c, ea, m);
    m = do_ror(c, m);
    w8(c, ea, m);
    do_adc(c, m);
}

/* 70: BVS */
static void op_70(struct cpu *c) { do_branch(c, (c->p & CPU_FLAG_V) != 0); }

/* 71: ADC (zp),Y */
static void op_71(struct cpu *c) { uint16_t ea = ea_izy_r(c); do_adc(c, r8(c, ea)); }

/* 73: RRA (zp),Y */
static void op_73(struct cpu *c)
{
    uint16_t ea = ea_izy_rw(c);
    uint8_t m = r8(c, ea);
    w8(c, ea, m);
    m = do_ror(c, m);
    w8(c, ea, m);
    do_adc(c, m);
}

/* 74: NOP zp,X */
static void op_74(struct cpu *c) { uint16_t ea = ea_zp_x(c); r8(c, ea); }

/* 75: ADC zp,X */
static void op_75(struct cpu *c) { uint16_t ea = ea_zp_x(c); do_adc(c, r8(c, ea)); }

/* 76: ROR zp,X */
static void op_76(struct cpu *c) { do_rmw(c, ea_zp_x(c), do_ror); }

/* 77: RRA zp,X */
static void op_77(struct cpu *c)
{
    uint16_t ea = ea_zp_x(c);
    uint8_t m = r8(c, ea);
    w8(c, ea, m);
    m = do_ror(c, m);
    w8(c, ea, m);
    do_adc(c, m);
}

/* 78: SEI */
static void op_78(struct cpu *c) { r8(c, c->pc); c->p |= CPU_FLAG_I; }

/* 79: ADC abs,Y */
static void op_79(struct cpu *c) { uint16_t ea = ea_abs_y_r(c); do_adc(c, r8(c, ea)); }

/* 7A: NOP */
static void op_7A(struct cpu *c) { r8(c, c->pc); }

/* 7B: RRA abs,Y */
static void op_7B(struct cpu *c)
{
    uint16_t ea = ea_abs_y_rw(c);
    uint8_t m = r8(c, ea);
    w8(c, ea, m);
    m = do_ror(c, m);
    w8(c, ea, m);
    do_adc(c, m);
}

/* 7C: NOP abs,X */
static void op_7C(struct cpu *c) { uint16_t ea = ea_abs_x_r(c); r8(c, ea); }

/* 7D: ADC abs,X */
static void op_7D(struct cpu *c) { uint16_t ea = ea_abs_x_r(c); do_adc(c, r8(c, ea)); }

/* 7E: ROR abs,X */
static void op_7E(struct cpu *c) { do_rmw(c, ea_abs_x_rw(c), do_ror); }

/* 7F: RRA abs,X */
static void op_7F(struct cpu *c)
{
    uint16_t ea = ea_abs_x_rw(c);
    uint8_t m = r8(c, ea);
    w8(c, ea, m);
    m = do_ror(c, m);
    w8(c, ea, m);
    do_adc(c, m);
}

/* 80: NOP #imm (undoc) */
static void op_80(struct cpu *c) { r8(c, c->pc++); }

/* 81: STA (zp,X) */
static void op_81(struct cpu *c) { uint16_t ea = ea_izx(c); w8(c, ea, c->a); }

/* 82: NOP #imm */
static void op_82(struct cpu *c) { r8(c, c->pc++); }

/* 83: SAX (zp,X) */
static void op_83(struct cpu *c) { uint16_t ea = ea_izx(c); w8(c, ea, (uint8_t)(c->a & c->x)); }

/* 84: STY zp */
static void op_84(struct cpu *c) { uint16_t ea = ea_zp(c); w8(c, ea, c->y); }

/* 85: STA zp */
static void op_85(struct cpu *c) { uint16_t ea = ea_zp(c); w8(c, ea, c->a); }

/* 86: STX zp */
static void op_86(struct cpu *c) { uint16_t ea = ea_zp(c); w8(c, ea, c->x); }

/* 87: SAX zp */
static void op_87(struct cpu *c) { uint16_t ea = ea_zp(c); w8(c, ea, (uint8_t)(c->a & c->x)); }

/* 88: DEY */
static void op_88(struct cpu *c) { r8(c, c->pc); c->y--; snz(c, c->y); }

/* 89: NOP #imm */
static void op_89(struct cpu *c) { r8(c, c->pc++); }

/* 8A: TXA */
static void op_8A(struct cpu *c) { r8(c, c->pc); c->a = c->x; snz(c, c->a); }

/* 8B: XAA / ANE #imm (unstable — deterministic: A = (A | magic) & X & imm) */
static void op_8B(struct cpu *c)
{
    uint8_t m = r8(c, c->pc++);
    c->a = (uint8_t)((c->a | 0xEE) & c->x & m);
    snz(c, c->a);
}

/* 8C: STY abs */
static void op_8C(struct cpu *c) { uint16_t ea = ea_abs(c); w8(c, ea, c->y); }

/* 8D: STA abs */
static void op_8D(struct cpu *c) { uint16_t ea = ea_abs(c); w8(c, ea, c->a); }

/* 8E: STX abs */
static void op_8E(struct cpu *c) { uint16_t ea = ea_abs(c); w8(c, ea, c->x); }

/* 8F: SAX abs */
static void op_8F(struct cpu *c) { uint16_t ea = ea_abs(c); w8(c, ea, (uint8_t)(c->a & c->x)); }

/* 90: BCC */
static void op_90(struct cpu *c) { do_branch(c, !(c->p & CPU_FLAG_C)); }

/* 91: STA (zp),Y */
static void op_91(struct cpu *c) { uint16_t ea = ea_izy_rw(c); w8(c, ea, c->a); }

/* 93: AHX (zp),Y (unstable) */
static void op_93(struct cpu *c)
{
    uint16_t ea = ea_izy_rw(c);
    w8(c, ea, (uint8_t)(c->a & c->x & (uint8_t)((ea >> 8) + 1)));
}

/* 94: STY zp,X */
static void op_94(struct cpu *c) { uint16_t ea = ea_zp_x(c); w8(c, ea, c->y); }

/* 95: STA zp,X */
static void op_95(struct cpu *c) { uint16_t ea = ea_zp_x(c); w8(c, ea, c->a); }

/* 96: STX zp,Y */
static void op_96(struct cpu *c) { uint16_t ea = ea_zp_y(c); w8(c, ea, c->x); }

/* 97: SAX zp,Y */
static void op_97(struct cpu *c) { uint16_t ea = ea_zp_y(c); w8(c, ea, (uint8_t)(c->a & c->x)); }

/* 98: TYA */
static void op_98(struct cpu *c) { r8(c, c->pc); c->a = c->y; snz(c, c->a); }

/* 99: STA abs,Y */
static void op_99(struct cpu *c) { uint16_t ea = ea_abs_y_rw(c); w8(c, ea, c->a); }

/* 9A: TXS */
static void op_9A(struct cpu *c) { r8(c, c->pc); c->s = c->x; }

/* 9B: TAS abs,Y (unstable) */
static void op_9B(struct cpu *c)
{
    uint16_t ea = ea_abs_y_rw(c);
    c->s = (uint8_t)(c->a & c->x);
    w8(c, ea, (uint8_t)(c->s & (uint8_t)((ea >> 8) + 1)));
}

/* 9C: SHY abs,X (unstable) */
static void op_9C(struct cpu *c)
{
    uint16_t ea = ea_abs_x_rw(c);
    w8(c, ea, (uint8_t)(c->y & (uint8_t)((ea >> 8) + 1)));
}

/* 9D: STA abs,X */
static void op_9D(struct cpu *c) { uint16_t ea = ea_abs_x_rw(c); w8(c, ea, c->a); }

/* 9E: SHX abs,Y (unstable) */
static void op_9E(struct cpu *c)
{
    uint16_t ea = ea_abs_y_rw(c);
    w8(c, ea, (uint8_t)(c->x & (uint8_t)((ea >> 8) + 1)));
}

/* 9F: AHX abs,Y (unstable) */
static void op_9F(struct cpu *c)
{
    uint16_t ea = ea_abs_y_rw(c);
    w8(c, ea, (uint8_t)(c->a & c->x & (uint8_t)((ea >> 8) + 1)));
}

/* A0: LDY #imm */
static void op_A0(struct cpu *c) { c->y = r8(c, c->pc++); snz(c, c->y); }

/* A1: LDA (zp,X) */
static void op_A1(struct cpu *c) { uint16_t ea = ea_izx(c); c->a = r8(c, ea); snz(c, c->a); }

/* A2: LDX #imm */
static void op_A2(struct cpu *c) { c->x = r8(c, c->pc++); snz(c, c->x); }

/* A3: LAX (zp,X) */
static void op_A3(struct cpu *c)
{
    uint16_t ea = ea_izx(c);
    uint8_t v = r8(c, ea);
    c->a = v; c->x = v;
    snz(c, v);
}

/* A4: LDY zp */
static void op_A4(struct cpu *c) { uint16_t ea = ea_zp(c); c->y = r8(c, ea); snz(c, c->y); }

/* A5: LDA zp */
static void op_A5(struct cpu *c) { uint16_t ea = ea_zp(c); c->a = r8(c, ea); snz(c, c->a); }

/* A6: LDX zp */
static void op_A6(struct cpu *c) { uint16_t ea = ea_zp(c); c->x = r8(c, ea); snz(c, c->x); }

/* A7: LAX zp */
static void op_A7(struct cpu *c)
{
    uint16_t ea = ea_zp(c);
    uint8_t v = r8(c, ea);
    c->a = v; c->x = v;
    snz(c, v);
}

/* A8: TAY */
static void op_A8(struct cpu *c) { r8(c, c->pc); c->y = c->a; snz(c, c->y); }

/* A9: LDA #imm */
static void op_A9(struct cpu *c) { c->a = r8(c, c->pc++); snz(c, c->a); }

/* AA: TAX */
static void op_AA(struct cpu *c) { r8(c, c->pc); c->x = c->a; snz(c, c->x); }

/* AB: LAX #imm (unstable / LXA) */
static void op_AB(struct cpu *c)
{
    uint8_t m = r8(c, c->pc++);
    uint8_t v = (uint8_t)((c->a | 0xEE) & m);
    c->a = v; c->x = v;
    snz(c, v);
}

/* AC: LDY abs */
static void op_AC(struct cpu *c) { uint16_t ea = ea_abs(c); c->y = r8(c, ea); snz(c, c->y); }

/* AD: LDA abs */
static void op_AD(struct cpu *c) { uint16_t ea = ea_abs(c); c->a = r8(c, ea); snz(c, c->a); }

/* AE: LDX abs */
static void op_AE(struct cpu *c) { uint16_t ea = ea_abs(c); c->x = r8(c, ea); snz(c, c->x); }

/* AF: LAX abs */
static void op_AF(struct cpu *c)
{
    uint16_t ea = ea_abs(c);
    uint8_t v = r8(c, ea);
    c->a = v; c->x = v;
    snz(c, v);
}

/* B0: BCS */
static void op_B0(struct cpu *c) { do_branch(c, (c->p & CPU_FLAG_C) != 0); }

/* B1: LDA (zp),Y */
static void op_B1(struct cpu *c) { uint16_t ea = ea_izy_r(c); c->a = r8(c, ea); snz(c, c->a); }

/* B3: LAX (zp),Y */
static void op_B3(struct cpu *c)
{
    uint16_t ea = ea_izy_r(c);
    uint8_t v = r8(c, ea);
    c->a = v; c->x = v;
    snz(c, v);
}

/* B4: LDY zp,X */
static void op_B4(struct cpu *c) { uint16_t ea = ea_zp_x(c); c->y = r8(c, ea); snz(c, c->y); }

/* B5: LDA zp,X */
static void op_B5(struct cpu *c) { uint16_t ea = ea_zp_x(c); c->a = r8(c, ea); snz(c, c->a); }

/* B6: LDX zp,Y */
static void op_B6(struct cpu *c) { uint16_t ea = ea_zp_y(c); c->x = r8(c, ea); snz(c, c->x); }

/* B7: LAX zp,Y */
static void op_B7(struct cpu *c)
{
    uint16_t ea = ea_zp_y(c);
    uint8_t v = r8(c, ea);
    c->a = v; c->x = v;
    snz(c, v);
}

/* B8: CLV */
static void op_B8(struct cpu *c) { r8(c, c->pc); c->p &= (uint8_t)~CPU_FLAG_V; }

/* B9: LDA abs,Y */
static void op_B9(struct cpu *c) { uint16_t ea = ea_abs_y_r(c); c->a = r8(c, ea); snz(c, c->a); }

/* BA: TSX */
static void op_BA(struct cpu *c) { r8(c, c->pc); c->x = c->s; snz(c, c->x); }

/* BB: LAS abs,Y (unstable-ish) */
static void op_BB(struct cpu *c)
{
    uint16_t ea = ea_abs_y_r(c);
    uint8_t v = (uint8_t)(r8(c, ea) & c->s);
    c->a = v; c->x = v; c->s = v;
    snz(c, v);
}

/* BC: LDY abs,X */
static void op_BC(struct cpu *c) { uint16_t ea = ea_abs_x_r(c); c->y = r8(c, ea); snz(c, c->y); }

/* BD: LDA abs,X */
static void op_BD(struct cpu *c) { uint16_t ea = ea_abs_x_r(c); c->a = r8(c, ea); snz(c, c->a); }

/* BE: LDX abs,Y */
static void op_BE(struct cpu *c) { uint16_t ea = ea_abs_y_r(c); c->x = r8(c, ea); snz(c, c->x); }

/* BF: LAX abs,Y */
static void op_BF(struct cpu *c)
{
    uint16_t ea = ea_abs_y_r(c);
    uint8_t v = r8(c, ea);
    c->a = v; c->x = v;
    snz(c, v);
}

/* C0: CPY #imm */
static void op_C0(struct cpu *c) { do_cmp(c, c->y, r8(c, c->pc++)); }

/* C1: CMP (zp,X) */
static void op_C1(struct cpu *c) { uint16_t ea = ea_izx(c); do_cmp(c, c->a, r8(c, ea)); }

/* C2: NOP #imm */
static void op_C2(struct cpu *c) { r8(c, c->pc++); }

/* C3: DCP (zp,X) */
static void op_C3(struct cpu *c)
{
    uint16_t ea = ea_izx(c);
    uint8_t m = r8(c, ea);
    w8(c, ea, m);
    m--;
    w8(c, ea, m);
    do_cmp(c, c->a, m);
}

/* C4: CPY zp */
static void op_C4(struct cpu *c) { uint16_t ea = ea_zp(c); do_cmp(c, c->y, r8(c, ea)); }

/* C5: CMP zp */
static void op_C5(struct cpu *c) { uint16_t ea = ea_zp(c); do_cmp(c, c->a, r8(c, ea)); }

/* C6: DEC zp */
static void op_C6(struct cpu *c)
{
    uint16_t ea = ea_zp(c);
    uint8_t m = r8(c, ea);
    w8(c, ea, m);
    m--;
    w8(c, ea, m);
    snz(c, m);
}

/* C7: DCP zp */
static void op_C7(struct cpu *c)
{
    uint16_t ea = ea_zp(c);
    uint8_t m = r8(c, ea);
    w8(c, ea, m);
    m--;
    w8(c, ea, m);
    do_cmp(c, c->a, m);
}

/* C8: INY */
static void op_C8(struct cpu *c) { r8(c, c->pc); c->y++; snz(c, c->y); }

/* C9: CMP #imm */
static void op_C9(struct cpu *c) { do_cmp(c, c->a, r8(c, c->pc++)); }

/* CA: DEX */
static void op_CA(struct cpu *c) { r8(c, c->pc); c->x--; snz(c, c->x); }

/* CB: AXS #imm (undoc) */
static void op_CB(struct cpu *c)
{
    uint8_t m = r8(c, c->pc++);
    unsigned ax = (unsigned)(c->a & c->x);
    unsigned sub = ax + (unsigned)(uint8_t)(~m) + 1u;
    c->x = (uint8_t)sub;
    set_flag(c, CPU_FLAG_C, sub > 0xFF);
    snz(c, c->x);
}

/* CC: CPY abs */
static void op_CC(struct cpu *c) { uint16_t ea = ea_abs(c); do_cmp(c, c->y, r8(c, ea)); }

/* CD: CMP abs */
static void op_CD(struct cpu *c) { uint16_t ea = ea_abs(c); do_cmp(c, c->a, r8(c, ea)); }

/* CE: DEC abs */
static void op_CE(struct cpu *c)
{
    uint16_t ea = ea_abs(c);
    uint8_t m = r8(c, ea);
    w8(c, ea, m);
    m--;
    w8(c, ea, m);
    snz(c, m);
}

/* CF: DCP abs */
static void op_CF(struct cpu *c)
{
    uint16_t ea = ea_abs(c);
    uint8_t m = r8(c, ea);
    w8(c, ea, m);
    m--;
    w8(c, ea, m);
    do_cmp(c, c->a, m);
}

/* D0: BNE */
static void op_D0(struct cpu *c) { do_branch(c, !(c->p & CPU_FLAG_Z)); }

/* D1: CMP (zp),Y */
static void op_D1(struct cpu *c) { uint16_t ea = ea_izy_r(c); do_cmp(c, c->a, r8(c, ea)); }

/* D3: DCP (zp),Y */
static void op_D3(struct cpu *c)
{
    uint16_t ea = ea_izy_rw(c);
    uint8_t m = r8(c, ea);
    w8(c, ea, m);
    m--;
    w8(c, ea, m);
    do_cmp(c, c->a, m);
}

/* D4: NOP zp,X */
static void op_D4(struct cpu *c) { uint16_t ea = ea_zp_x(c); r8(c, ea); }

/* D5: CMP zp,X */
static void op_D5(struct cpu *c) { uint16_t ea = ea_zp_x(c); do_cmp(c, c->a, r8(c, ea)); }

/* D6: DEC zp,X */
static void op_D6(struct cpu *c)
{
    uint16_t ea = ea_zp_x(c);
    uint8_t m = r8(c, ea);
    w8(c, ea, m);
    m--;
    w8(c, ea, m);
    snz(c, m);
}

/* D7: DCP zp,X */
static void op_D7(struct cpu *c)
{
    uint16_t ea = ea_zp_x(c);
    uint8_t m = r8(c, ea);
    w8(c, ea, m);
    m--;
    w8(c, ea, m);
    do_cmp(c, c->a, m);
}

/* D8: CLD */
static void op_D8(struct cpu *c) { r8(c, c->pc); c->p &= (uint8_t)~CPU_FLAG_D; }

/* D9: CMP abs,Y */
static void op_D9(struct cpu *c) { uint16_t ea = ea_abs_y_r(c); do_cmp(c, c->a, r8(c, ea)); }

/* DA: NOP */
static void op_DA(struct cpu *c) { r8(c, c->pc); }

/* DB: DCP abs,Y */
static void op_DB(struct cpu *c)
{
    uint16_t ea = ea_abs_y_rw(c);
    uint8_t m = r8(c, ea);
    w8(c, ea, m);
    m--;
    w8(c, ea, m);
    do_cmp(c, c->a, m);
}

/* DC: NOP abs,X */
static void op_DC(struct cpu *c) { uint16_t ea = ea_abs_x_r(c); r8(c, ea); }

/* DD: CMP abs,X */
static void op_DD(struct cpu *c) { uint16_t ea = ea_abs_x_r(c); do_cmp(c, c->a, r8(c, ea)); }

/* DE: DEC abs,X */
static void op_DE(struct cpu *c)
{
    uint16_t ea = ea_abs_x_rw(c);
    uint8_t m = r8(c, ea);
    w8(c, ea, m);
    m--;
    w8(c, ea, m);
    snz(c, m);
}

/* DF: DCP abs,X */
static void op_DF(struct cpu *c)
{
    uint16_t ea = ea_abs_x_rw(c);
    uint8_t m = r8(c, ea);
    w8(c, ea, m);
    m--;
    w8(c, ea, m);
    do_cmp(c, c->a, m);
}

/* E0: CPX #imm */
static void op_E0(struct cpu *c) { do_cmp(c, c->x, r8(c, c->pc++)); }

/* E1: SBC (zp,X) */
static void op_E1(struct cpu *c) { uint16_t ea = ea_izx(c); do_sbc(c, r8(c, ea)); }

/* E2: NOP #imm */
static void op_E2(struct cpu *c) { r8(c, c->pc++); }

/* E3: ISC (zp,X) */
static void op_E3(struct cpu *c)
{
    uint16_t ea = ea_izx(c);
    uint8_t m = r8(c, ea);
    w8(c, ea, m);
    m++;
    w8(c, ea, m);
    do_sbc(c, m);
}

/* E4: CPX zp */
static void op_E4(struct cpu *c) { uint16_t ea = ea_zp(c); do_cmp(c, c->x, r8(c, ea)); }

/* E5: SBC zp */
static void op_E5(struct cpu *c) { uint16_t ea = ea_zp(c); do_sbc(c, r8(c, ea)); }

/* E6: INC zp */
static void op_E6(struct cpu *c)
{
    uint16_t ea = ea_zp(c);
    uint8_t m = r8(c, ea);
    w8(c, ea, m);
    m++;
    w8(c, ea, m);
    snz(c, m);
}

/* E7: ISC zp */
static void op_E7(struct cpu *c)
{
    uint16_t ea = ea_zp(c);
    uint8_t m = r8(c, ea);
    w8(c, ea, m);
    m++;
    w8(c, ea, m);
    do_sbc(c, m);
}

/* E8: INX */
static void op_E8(struct cpu *c) { r8(c, c->pc); c->x++; snz(c, c->x); }

/* E9: SBC #imm */
static void op_E9(struct cpu *c) { do_sbc(c, r8(c, c->pc++)); }

/* EA: NOP */
static void op_EA(struct cpu *c) { r8(c, c->pc); }

/* EB: SBC #imm (undoc, same as E9) */
static void op_EB(struct cpu *c) { do_sbc(c, r8(c, c->pc++)); }

/* EC: CPX abs */
static void op_EC(struct cpu *c) { uint16_t ea = ea_abs(c); do_cmp(c, c->x, r8(c, ea)); }

/* ED: SBC abs */
static void op_ED(struct cpu *c) { uint16_t ea = ea_abs(c); do_sbc(c, r8(c, ea)); }

/* EE: INC abs */
static void op_EE(struct cpu *c)
{
    uint16_t ea = ea_abs(c);
    uint8_t m = r8(c, ea);
    w8(c, ea, m);
    m++;
    w8(c, ea, m);
    snz(c, m);
}

/* EF: ISC abs */
static void op_EF(struct cpu *c)
{
    uint16_t ea = ea_abs(c);
    uint8_t m = r8(c, ea);
    w8(c, ea, m);
    m++;
    w8(c, ea, m);
    do_sbc(c, m);
}

/* F0: BEQ */
static void op_F0(struct cpu *c) { do_branch(c, (c->p & CPU_FLAG_Z) != 0); }

/* F1: SBC (zp),Y */
static void op_F1(struct cpu *c) { uint16_t ea = ea_izy_r(c); do_sbc(c, r8(c, ea)); }

/* F3: ISC (zp),Y */
static void op_F3(struct cpu *c)
{
    uint16_t ea = ea_izy_rw(c);
    uint8_t m = r8(c, ea);
    w8(c, ea, m);
    m++;
    w8(c, ea, m);
    do_sbc(c, m);
}

/* F4: NOP zp,X */
static void op_F4(struct cpu *c) { uint16_t ea = ea_zp_x(c); r8(c, ea); }

/* F5: SBC zp,X */
static void op_F5(struct cpu *c) { uint16_t ea = ea_zp_x(c); do_sbc(c, r8(c, ea)); }

/* F6: INC zp,X */
static void op_F6(struct cpu *c)
{
    uint16_t ea = ea_zp_x(c);
    uint8_t m = r8(c, ea);
    w8(c, ea, m);
    m++;
    w8(c, ea, m);
    snz(c, m);
}

/* F7: ISC zp,X */
static void op_F7(struct cpu *c)
{
    uint16_t ea = ea_zp_x(c);
    uint8_t m = r8(c, ea);
    w8(c, ea, m);
    m++;
    w8(c, ea, m);
    do_sbc(c, m);
}

/* F8: SED */
static void op_F8(struct cpu *c) { r8(c, c->pc); c->p |= CPU_FLAG_D; }

/* F9: SBC abs,Y */
static void op_F9(struct cpu *c) { uint16_t ea = ea_abs_y_r(c); do_sbc(c, r8(c, ea)); }

/* FA: NOP */
static void op_FA(struct cpu *c) { r8(c, c->pc); }

/* FB: ISC abs,Y */
static void op_FB(struct cpu *c)
{
    uint16_t ea = ea_abs_y_rw(c);
    uint8_t m = r8(c, ea);
    w8(c, ea, m);
    m++;
    w8(c, ea, m);
    do_sbc(c, m);
}

/* FC: NOP abs,X */
static void op_FC(struct cpu *c) { uint16_t ea = ea_abs_x_r(c); r8(c, ea); }

/* FD: SBC abs,X */
static void op_FD(struct cpu *c) { uint16_t ea = ea_abs_x_r(c); do_sbc(c, r8(c, ea)); }

/* FE: INC abs,X */
static void op_FE(struct cpu *c)
{
    uint16_t ea = ea_abs_x_rw(c);
    uint8_t m = r8(c, ea);
    w8(c, ea, m);
    m++;
    w8(c, ea, m);
    snz(c, m);
}

/* FF: ISC abs,X */
static void op_FF(struct cpu *c)
{
    uint16_t ea = ea_abs_x_rw(c);
    uint8_t m = r8(c, ea);
    w8(c, ea, m);
    m++;
    w8(c, ea, m);
    do_sbc(c, m);
}

/* ============================================================
 *   Dispatch table
 * ============================================================ */

typedef void (*op_fn)(struct cpu *c);

static const op_fn dispatch[256] = {
    /* 00 */ op_00,   op_01,   op_jam,  op_03,   op_04,   op_05,   op_06,   op_07,
    /* 08 */ op_08,   op_09,   op_0A,   op_0B,   op_0C,   op_0D,   op_0E,   op_0F,
    /* 10 */ op_10,   op_11,   op_jam,  op_13,   op_14,   op_15,   op_16,   op_17,
    /* 18 */ op_18,   op_19,   op_1A,   op_1B,   op_1C,   op_1D,   op_1E,   op_1F,
    /* 20 */ op_20,   op_21,   op_jam,  op_23,   op_24,   op_25,   op_26,   op_27,
    /* 28 */ op_28,   op_29,   op_2A,   op_2B,   op_2C,   op_2D,   op_2E,   op_2F,
    /* 30 */ op_30,   op_31,   op_jam,  op_33,   op_34,   op_35,   op_36,   op_37,
    /* 38 */ op_38,   op_39,   op_3A,   op_3B,   op_3C,   op_3D,   op_3E,   op_3F,
    /* 40 */ op_40,   op_41,   op_jam,  op_43,   op_44,   op_45,   op_46,   op_47,
    /* 48 */ op_48,   op_49,   op_4A,   op_4B,   op_4C,   op_4D,   op_4E,   op_4F,
    /* 50 */ op_50,   op_51,   op_jam,  op_53,   op_54,   op_55,   op_56,   op_57,
    /* 58 */ op_58,   op_59,   op_5A,   op_5B,   op_5C,   op_5D,   op_5E,   op_5F,
    /* 60 */ op_60,   op_61,   op_jam,  op_63,   op_64,   op_65,   op_66,   op_67,
    /* 68 */ op_68,   op_69,   op_6A,   op_6B,   op_6C,   op_6D,   op_6E,   op_6F,
    /* 70 */ op_70,   op_71,   op_jam,  op_73,   op_74,   op_75,   op_76,   op_77,
    /* 78 */ op_78,   op_79,   op_7A,   op_7B,   op_7C,   op_7D,   op_7E,   op_7F,
    /* 80 */ op_80,   op_81,   op_82,   op_83,   op_84,   op_85,   op_86,   op_87,
    /* 88 */ op_88,   op_89,   op_8A,   op_8B,   op_8C,   op_8D,   op_8E,   op_8F,
    /* 90 */ op_90,   op_91,   op_jam,  op_93,   op_94,   op_95,   op_96,   op_97,
    /* 98 */ op_98,   op_99,   op_9A,   op_9B,   op_9C,   op_9D,   op_9E,   op_9F,
    /* A0 */ op_A0,   op_A1,   op_A2,   op_A3,   op_A4,   op_A5,   op_A6,   op_A7,
    /* A8 */ op_A8,   op_A9,   op_AA,   op_AB,   op_AC,   op_AD,   op_AE,   op_AF,
    /* B0 */ op_B0,   op_B1,   op_jam,  op_B3,   op_B4,   op_B5,   op_B6,   op_B7,
    /* B8 */ op_B8,   op_B9,   op_BA,   op_BB,   op_BC,   op_BD,   op_BE,   op_BF,
    /* C0 */ op_C0,   op_C1,   op_C2,   op_C3,   op_C4,   op_C5,   op_C6,   op_C7,
    /* C8 */ op_C8,   op_C9,   op_CA,   op_CB,   op_CC,   op_CD,   op_CE,   op_CF,
    /* D0 */ op_D0,   op_D1,   op_jam,  op_D3,   op_D4,   op_D5,   op_D6,   op_D7,
    /* D8 */ op_D8,   op_D9,   op_DA,   op_DB,   op_DC,   op_DD,   op_DE,   op_DF,
    /* E0 */ op_E0,   op_E1,   op_E2,   op_E3,   op_E4,   op_E5,   op_E6,   op_E7,
    /* E8 */ op_E8,   op_E9,   op_EA,   op_EB,   op_EC,   op_ED,   op_EE,   op_EF,
    /* F0 */ op_F0,   op_F1,   op_jam,  op_F3,   op_F4,   op_F5,   op_F6,   op_F7,
    /* F8 */ op_F8,   op_F9,   op_FA,   op_FB,   op_FC,   op_FD,   op_FE,   op_FF
};

/* ============================================================
 *   Public API
 * ============================================================ */

void cpu_init(struct cpu *c, struct cpu_bus bus)
{
    memset(c, 0, sizeof(*c));
    c->bus = bus;
}

void cpu_reset(struct cpu *c)
{
    uint8_t lo, hi;
    c->halted = false;
    /* 7-cycle reset: 2 dummy fetches, 3 stack dummy reads, 2 vector reads */
    r8(c, c->pc);
    r8(c, c->pc);
    r8(c, (uint16_t)(0x0100 | c->s)); c->s--;
    r8(c, (uint16_t)(0x0100 | c->s)); c->s--;
    r8(c, (uint16_t)(0x0100 | c->s)); c->s--;
    lo = r8(c, 0xFFFC);
    hi = r8(c, 0xFFFD);
    c->pc = (uint16_t)((uint16_t)lo | ((uint16_t)hi << 8));
    c->p |= (uint8_t)(CPU_FLAG_I | CPU_FLAG_U);
}

void cpu_step(struct cpu *c)
{
    uint8_t op;
    if (c->halted) return;
    op = r8(c, c->pc++);
    dispatch[op](c);
}

size_t cpu_serialize_size(void)
{
    return 1 + 1 + 1 + 1 + 1 + 2 + 1 + 8;  /* a x y s p pc halted cycles */
}

void cpu_serialize(const struct cpu *c, void *buf)
{
    uint8_t *p = (uint8_t *)buf;
    p[0]  = c->a;
    p[1]  = c->x;
    p[2]  = c->y;
    p[3]  = c->s;
    p[4]  = c->p;
    p[5]  = (uint8_t)(c->pc & 0xFF);
    p[6]  = (uint8_t)(c->pc >> 8);
    p[7]  = c->halted ? 1u : 0u;
    p[8]  = (uint8_t)(c->cycles);
    p[9]  = (uint8_t)(c->cycles >> 8);
    p[10] = (uint8_t)(c->cycles >> 16);
    p[11] = (uint8_t)(c->cycles >> 24);
    p[12] = (uint8_t)(c->cycles >> 32);
    p[13] = (uint8_t)(c->cycles >> 40);
    p[14] = (uint8_t)(c->cycles >> 48);
    p[15] = (uint8_t)(c->cycles >> 56);
}

bool cpu_deserialize(struct cpu *c, const void *buf, size_t size)
{
    const uint8_t *p = (const uint8_t *)buf;
    if (size < cpu_serialize_size()) return false;
    c->a  = p[0];
    c->x  = p[1];
    c->y  = p[2];
    c->s  = p[3];
    c->p  = p[4];
    c->pc = (uint16_t)((uint16_t)p[5] | ((uint16_t)p[6] << 8));
    c->halted = p[7] != 0;
    c->cycles =  (uint64_t)p[8]
              | ((uint64_t)p[9]  << 8)
              | ((uint64_t)p[10] << 16)
              | ((uint64_t)p[11] << 24)
              | ((uint64_t)p[12] << 32)
              | ((uint64_t)p[13] << 40)
              | ((uint64_t)p[14] << 48)
              | ((uint64_t)p[15] << 56);
    return true;
}
