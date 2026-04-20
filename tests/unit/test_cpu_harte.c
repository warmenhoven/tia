/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Exhaustive per-instruction cycle-accurate validation using the Tom Harte
 * (SingleStepTests/65x02) test vectors for the NMOS 6502. Each vector
 * specifies the CPU's initial state, the expected final state, and the
 * full per-cycle bus-access stream for a single instruction. We replay
 * each vector through our CPU module and verify that registers, flags,
 * modified RAM, and every bus access match exactly.
 *
 * The vector corpus is fetched on demand by tests/tools/fetch_harte.py —
 * it's ~220 MB, not checked into git. If it's absent, this test prints
 * how to fetch it and exits without failing the suite.
 *
 * The packed binary format is documented in fetch_harte.py. */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu.h"

/* ------------------------------------------------------------------
 *  A full flat 64KB RAM + per-access log. The Harte vectors expect a
 *  bus where every address responds, so we give the CPU a plain array.
 * ------------------------------------------------------------------ */

#define MAX_CYCLES 16   /* any 6502 instruction fits in 7 cycles; safety margin */

struct bus_log_entry {
    uint16_t addr;
    uint8_t  val;
    uint8_t  rw;   /* 0 = read, 1 = write */
};

struct fake_bus {
    uint8_t              ram[65536];
    struct bus_log_entry log[MAX_CYCLES];
    int                  log_n;
    int                  log_overflow;
};

static uint8_t fake_read(void *ctx, uint16_t addr)
{
    struct fake_bus *b = (struct fake_bus *)ctx;
    uint8_t v = b->ram[addr];
    if (b->log_n < MAX_CYCLES) {
        b->log[b->log_n].addr = addr;
        b->log[b->log_n].val  = v;
        b->log[b->log_n].rw   = 0;
        b->log_n++;
    } else {
        b->log_overflow = 1;
    }
    return v;
}

static void fake_write(void *ctx, uint16_t addr, uint8_t v)
{
    struct fake_bus *b = (struct fake_bus *)ctx;
    b->ram[addr] = v;
    if (b->log_n < MAX_CYCLES) {
        b->log[b->log_n].addr = addr;
        b->log[b->log_n].val  = v;
        b->log[b->log_n].rw   = 1;
        b->log_n++;
    } else {
        b->log_overflow = 1;
    }
}

/* ------------------------------------------------------------------
 *  Binary reader — walks the packed corpus sequentially. No allocs.
 * ------------------------------------------------------------------ */

struct reader {
    const uint8_t *p;
    const uint8_t *end;
    int            bad;
};

static uint8_t  rd_u8 (struct reader *r) {
    if (r->p + 1 > r->end) { r->bad = 1; return 0; }
    return *r->p++;
}
static uint16_t rd_u16(struct reader *r) {
    uint16_t v;
    if (r->p + 2 > r->end) { r->bad = 1; return 0; }
    v = (uint16_t)(r->p[0] | ((uint16_t)r->p[1] << 8));
    r->p += 2;
    return v;
}
static uint32_t rd_u32(struct reader *r) {
    uint32_t v;
    if (r->p + 4 > r->end) { r->bad = 1; return 0; }
    v =  (uint32_t)r->p[0]       | ((uint32_t)r->p[1] << 8)
      | ((uint32_t)r->p[2] << 16) | ((uint32_t)r->p[3] << 24);
    r->p += 4;
    return v;
}

/* ------------------------------------------------------------------
 *  One test case. Returns 0 on pass, non-zero on fail; diagnostic is
 *  printed to stderr in the failure path.
 * ------------------------------------------------------------------ */

struct expected_cycle {
    uint16_t addr;
    uint8_t  val;
    uint8_t  rw;
};

static int run_case(uint8_t op, int case_index, struct reader *r)
{
    struct cpu        c;
    struct cpu_bus    bus;
    struct fake_bus   fb;
    uint16_t init_pc;
    uint8_t  init_s, init_a, init_x, init_y, init_p;
    uint16_t fin_pc;
    uint8_t  fin_s, fin_a, fin_x, fin_y, fin_p;
    uint8_t  n_init_ram, n_final_ram, n_cycles;
    uint16_t touched_addrs[256];     /* RAM cells this case mentions */
    int      n_touched = 0;
    struct expected_cycle exp[MAX_CYCLES];
    int      i;

    /* --- Initial registers ------------------------------------------------ */
    init_pc = rd_u16(r);
    init_s  = rd_u8(r);
    init_a  = rd_u8(r);
    init_x  = rd_u8(r);
    init_y  = rd_u8(r);
    init_p  = rd_u8(r);

    /* --- Initial RAM: set these bytes in the fake bus ------------------- */
    memset(&fb, 0, sizeof(fb));
    n_init_ram = rd_u8(r);
    for (i = 0; i < n_init_ram; i++) {
        uint16_t a = rd_u16(r);
        uint8_t  v = rd_u8(r);
        fb.ram[a] = v;
        if (n_touched < 256) touched_addrs[n_touched++] = a;
    }

    /* --- Final registers (expected) -------------------------------------- */
    fin_pc = rd_u16(r);
    fin_s  = rd_u8(r);
    fin_a  = rd_u8(r);
    fin_x  = rd_u8(r);
    fin_y  = rd_u8(r);
    fin_p  = rd_u8(r);

    /* --- Final RAM (expected): compare against our ram[] after the step -- */
    /*   We don't store these up-front because they share many bytes with
     *   initial RAM; we record addr+value and verify by table lookup.      */
    n_final_ram = rd_u8(r);
    {
        uint16_t final_addrs[256];
        uint8_t  final_vals[256];
        for (i = 0; i < n_final_ram && i < 256; i++) {
            final_addrs[i] = rd_u16(r);
            final_vals[i]  = rd_u8(r);
        }
        /* Consume any remaining beyond our buffer (rare — tests cap ≤20). */
        for (; i < n_final_ram; i++) { (void)rd_u16(r); (void)rd_u8(r); }

        /* --- Expected cycle stream ------------------------------------------- */
        n_cycles = rd_u8(r);
        if (n_cycles > MAX_CYCLES) {
            fprintf(stderr,
                "op=0x%02X case=%d: harte reports %d cycles, MAX_CYCLES=%d\n",
                op, case_index, n_cycles, MAX_CYCLES);
            /* Still consume them so the reader stays aligned */
            for (i = 0; i < n_cycles; i++) { rd_u16(r); rd_u8(r); rd_u8(r); }
            return 1;
        }
        for (i = 0; i < n_cycles; i++) {
            exp[i].addr = rd_u16(r);
            exp[i].val  = rd_u8(r);
            exp[i].rw   = rd_u8(r);
        }

        if (r->bad) return 1;

        /* --- Run the CPU --------------------------------------------------- */
        bus.read  = fake_read;
        bus.write = fake_write;
        bus.ctx   = &fb;
        cpu_init(&c, bus);
        c.pc      = init_pc;
        c.s       = init_s;
        c.a       = init_a;
        c.x       = init_x;
        c.y       = init_y;
        c.p       = init_p;
        c.halted  = 0;
        c.cycles  = 0;
        fb.log_n        = 0;
        fb.log_overflow = 0;
        cpu_step(&c);

        /* --- Verify registers --------------------------------------------- */
        if (c.pc != fin_pc || c.s != fin_s || c.a != fin_a ||
            c.x  != fin_x  || c.y != fin_y || c.p != fin_p) {
            fprintf(stderr,
                "op=0x%02X case=%d regs: "
                "init PC=%04X A=%02X X=%02X Y=%02X S=%02X P=%02X; "
                "expected PC=%04X A=%02X X=%02X Y=%02X S=%02X P=%02X, "
                "got PC=%04X A=%02X X=%02X Y=%02X S=%02X P=%02X\n",
                op, case_index,
                init_pc, init_a, init_x, init_y, init_s, init_p,
                fin_pc, fin_a, fin_x, fin_y, fin_s, fin_p,
                c.pc,   c.a,   c.x,   c.y,   c.s,   c.p);
            return 1;
        }

        /* --- Verify cycle stream ------------------------------------------ */
        if (fb.log_overflow) {
            fprintf(stderr, "op=0x%02X case=%d: log overflow (>%d cycles)\n",
                    op, case_index, MAX_CYCLES);
            return 1;
        }
        if (fb.log_n != n_cycles) {
            fprintf(stderr,
                "op=0x%02X case=%d cycles: expected %d, got %d\n",
                op, case_index, n_cycles, fb.log_n);
            return 1;
        }
        for (i = 0; i < n_cycles; i++) {
            if (fb.log[i].addr != exp[i].addr ||
                fb.log[i].val  != exp[i].val  ||
                fb.log[i].rw   != exp[i].rw) {
                fprintf(stderr,
                    "op=0x%02X case=%d cycle %d: "
                    "init A=%02X X=%02X Y=%02X P=%02X; "
                    "expected (%04X, %02X, %s), got (%04X, %02X, %s)\n",
                    op, case_index, i,
                    init_a, init_x, init_y, init_p,
                    exp[i].addr, exp[i].val, exp[i].rw ? "W" : "R",
                    fb.log[i].addr, fb.log[i].val,
                    fb.log[i].rw ? "W" : "R");
                return 1;
            }
        }

        /* --- Verify RAM reachable from touched addresses ------------------ */
        /* Only cells the test explicitly mentions matter; others are
         * untouched by definition. For each final RAM entry, check ram[].  */
        for (i = 0; i < n_final_ram && i < 256; i++) {
            if (fb.ram[final_addrs[i]] != final_vals[i]) {
                fprintf(stderr,
                    "op=0x%02X case=%d ram: [%04X] expected %02X, got %02X\n",
                    op, case_index, final_addrs[i],
                    final_vals[i], fb.ram[final_addrs[i]]);
                return 1;
            }
        }
    }
    return 0;
}

/* ------------------------------------------------------------------
 *  Main
 * ------------------------------------------------------------------ */

#define DEFAULT_path "roms/harte_65x02.bin"

int main(int argc, char **argv)
{
    FILE          *f;
    long           file_size;
    uint8_t       *data;
    struct reader  r;
    uint32_t       version, n_opcodes;
    int            op_i;
    int            total_cases = 0, total_failures = 0;
    int            per_op_max_failures = 5;   /* don't spam stderr */
    const char    *path = (argc > 1) ? argv[1] : DEFAULT_path;

    f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr,
            "SKIP: %s not found. Fetch with:\n"
            "    python3 tests/tools/fetch_harte.py\n",
            path);
        /* autotools "skip" convention so make test doesn't fail */
        return 77;
    }

    if (fseek(f, 0, SEEK_END) != 0) { perror("fseek"); fclose(f); return 1; }
    file_size = ftell(f);
    if (file_size < 12) {
        fprintf(stderr, "corpus too small: %ld bytes\n", file_size);
        fclose(f);
        return 1;
    }
    rewind(f);
    data = (uint8_t *)malloc((size_t)file_size);
    if (!data) { perror("malloc"); fclose(f); return 1; }
    if (fread(data, 1, (size_t)file_size, f) != (size_t)file_size) {
        perror("fread"); free(data); fclose(f); return 1;
    }
    fclose(f);

    if (data[0] != 'H' || data[1] != 'A' ||
        data[2] != 'R' || data[3] != 'T') {
        fprintf(stderr, "bad magic in %s\n", path);
        free(data); return 1;
    }
    r.p = data + 4;
    r.end = data + file_size;
    r.bad = 0;
    version   = rd_u32(&r);
    n_opcodes = rd_u32(&r);
    if (version != 1) {
        fprintf(stderr, "unsupported corpus version %u\n", version);
        free(data); return 1;
    }
    printf("running Harte vectors: %u opcodes from %s (%ld bytes)\n",
           n_opcodes, path, file_size);

    for (op_i = 0; op_i < (int)n_opcodes; op_i++) {
        uint8_t   op = rd_u8(&r);
        uint32_t  n  = rd_u32(&r);
        int       op_failures = 0;
        uint32_t  ci;
        for (ci = 0; ci < n; ci++) {
            int before_bad = r.bad;
            int rc = run_case(op, (int)ci, &r);
            total_cases++;
            if (r.bad != before_bad) {
                fprintf(stderr,
                    "op=0x%02X case=%u: corrupt corpus (reader went bad)\n",
                    op, ci);
                free(data); return 1;
            }
            if (rc) {
                op_failures++;
                total_failures++;
                if (op_failures == per_op_max_failures) {
                    fprintf(stderr,
                        "op=0x%02X: reached %d failures, suppressing further\n",
                        op, per_op_max_failures);
                }
            }
        }
        if (op_failures > 0) {
            printf("  op 0x%02X: %d/%u FAILED\n", op, op_failures, n);
        }
    }

    printf("Harte vectors: %d cases, %d failures\n", total_cases, total_failures);
    free(data);
    return total_failures == 0 ? 0 : 1;
}
