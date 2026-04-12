/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cpu.h"

#define ROM_PATH    "roms/6502_functional_test.bin"
#define ENTRY_PC    0x0400
#define SUCCESS_PC  0x3469
#define MAX_STEPS   200000000L

static uint8_t ram[65536];

static uint8_t ram_read(void *ctx, uint16_t a)
{
    (void)ctx;
    return ram[a];
}

static void ram_write(void *ctx, uint16_t a, uint8_t v)
{
    (void)ctx;
    ram[a] = v;
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : ROM_PATH;
    struct cpu_bus bus;
    struct cpu c;
    FILE *f;
    size_t n;
    long steps;

    f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "cannot open %s\n", path);
        return 2;
    }
    n = fread(ram, 1, sizeof(ram), f);
    fclose(f);
    if (n != sizeof(ram)) {
        fprintf(stderr, "%s: expected %zu bytes, got %zu\n",
                path, sizeof(ram), n);
        return 2;
    }

    bus.read  = ram_read;
    bus.write = ram_write;
    bus.ctx   = NULL;
    cpu_init(&c, bus);
    c.pc = ENTRY_PC;
    c.s = 0xFD;
    c.p = (uint8_t)(CPU_FLAG_I | CPU_FLAG_U);

    for (steps = 0; steps < MAX_STEPS; steps++) {
        uint16_t pc_before = c.pc;
        cpu_step(&c);
        if (c.halted) {
            fprintf(stderr,
                    "FAIL: CPU halted (JAM) at PC=$%04X after %ld steps\n",
                    c.pc, steps);
            return 1;
        }
        if (c.pc == pc_before) {
            if (c.pc == SUCCESS_PC) {
                printf("PASS Dormann 6502_functional_test: PC=$%04X, "
                       "%ld steps, %llu cycles\n",
                       c.pc, steps, (unsigned long long)c.cycles);
                return 0;
            }
            fprintf(stderr,
                    "FAIL: trap at PC=$%04X after %ld steps "
                    "(expected success PC=$%04X)\n",
                    c.pc, steps, SUCCESS_PC);
            return 1;
        }
    }
    fprintf(stderr, "TIMEOUT after %ld steps at PC=$%04X\n", MAX_STEPS, c.pc);
    return 1;
}
