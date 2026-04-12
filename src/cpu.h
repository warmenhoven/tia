/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef TIA_CPU_H
#define TIA_CPU_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

enum {
    CPU_FLAG_C = 0x01,
    CPU_FLAG_Z = 0x02,
    CPU_FLAG_I = 0x04,
    CPU_FLAG_D = 0x08,
    CPU_FLAG_B = 0x10,
    CPU_FLAG_U = 0x20,
    CPU_FLAG_V = 0x40,
    CPU_FLAG_N = 0x80
};

/* The bus callbacks are invoked exactly once per CPU cycle.
 * The embedder is responsible for advancing other clocked components
 * (e.g. TIA at 3 color clocks per CPU cycle) and honoring RDY stalls
 * inside these callbacks. */
struct cpu_bus {
    uint8_t (*read)(void *ctx, uint16_t addr);
    void    (*write)(void *ctx, uint16_t addr, uint8_t data);
    void    *ctx;
};

struct cpu {
    uint8_t  a, x, y, s, p;
    uint16_t pc;
    bool     halted;
    uint64_t cycles;
    struct cpu_bus bus;
};

void cpu_init(struct cpu *c, struct cpu_bus bus);
void cpu_reset(struct cpu *c);
void cpu_step(struct cpu *c);

size_t cpu_serialize_size(void);
void   cpu_serialize(const struct cpu *c, void *buf);
bool   cpu_deserialize(struct cpu *c, const void *buf, size_t size);

#endif
