/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Sim2600 oracle counterpart: load a 2600 ROM into our TIA/CPU/bus/cart/
 * RIOT, run N TIA half-clocks, and emit an NDJSON trace in the same
 * shape as tests/tools/run_sim2600.py. tests/tools/compare_traces.py then
 * diffs the two files to pin down the first clock at which our
 * implementation deviates from the transistor-level oracle. */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cart.h"
#include "cpu.h"
#include "riot.h"
#include "tia.h"

struct harness {
    struct cpu  cpu;
    struct tia  tia;
    struct riot riot;
    struct cart cart;

    FILE    *trace;
    uint64_t hc;           /* TIA half-clock count (2 per color-clock tick) */
    uint64_t target_hc;    /* stop after this many half-clocks */
    uint64_t every;        /* emit one record every N half-clocks */
};

static void emit_record(struct harness *h)
{
    uint8_t ci  = h->tia.last_colulum;
    uint8_t lum = (uint8_t)(ci & 0x7);
    uint8_t col = (uint8_t)((ci >> 3) & 0xF);
    uint32_t rgba = h->tia.palette[ci];
    /* Trace's `rdy` follows Sim2600: 1 = RDY pulled high (not stalled);
     * `wsync` reports the WSYNC latch state which drives the stall. */
    int wsync = h->tia.rdy_asserted ? 1 : 0;
    int rdy   = wsync ? 0 : 1;

    fprintf(h->trace,
        "{\"hc\": %llu, \"cpu_hc\": %llu, "
        "\"vsync\": %d, \"vblank\": %d, \"wsync\": %d, \"rdy\": %d, "
        "\"lum\": %u, \"col\": %u, \"rgba\": %u}\n",
        (unsigned long long)h->hc,
        (unsigned long long)(h->cpu.cycles * 2ULL),
        h->tia.vsync ? 1 : 0, h->tia.vblank ? 1 : 0, wsync, rdy,
        (unsigned)lum, (unsigned)col, (unsigned)rgba);
}

static void h_tia_tick(struct harness *h)
{
    tia_tick(&h->tia);
    h->hc += 2;
    if ((h->hc % h->every) == 0)
        emit_record(h);
}

static void h_cycle(struct harness *h)
{
    h_tia_tick(h);
    h_tia_tick(h);
    h_tia_tick(h);
    riot_tick(&h->riot);
}

/* --- Bus dispatch — mirrors src/bus.c, but ticks through h_cycle so each
 *     TIA tick gets a trace record. --- */
static uint8_t h_bus_read(void *ctx, uint16_t addr)
{
    struct harness *h = (struct harness *)ctx;
    uint16_t a;
    uint8_t v;
    while (h->tia.rdy_asserted)
        h_cycle(h);
    h_cycle(h);
    a = (uint16_t)(addr & 0x1FFF);
    if (a & 0x1000)       v = cart_read(&h->cart, a);
    else if (a & 0x0080)  v = riot_read(&h->riot, a);
    else                  v = tia_read(&h->tia, a);
    cart_snoop_bus(&h->cart, a, v);
    return v;
}

static void h_bus_write(void *ctx, uint16_t addr, uint8_t data)
{
    struct harness *h = (struct harness *)ctx;
    uint16_t a;
    h_tia_tick(h);
    h_tia_tick(h);
    h_tia_tick(h);
    riot_tick(&h->riot);
    a = (uint16_t)(addr & 0x1FFF);
    cart_snoop_write(&h->cart, a, data);
    if (a & 0x1000)        cart_write(&h->cart, a, data);
    else if (a & 0x0080)   riot_write(&h->riot, a, data);
    else                   tia_write(&h->tia, a, data);
    cart_snoop_bus(&h->cart, a, data);
}

static int load_rom(struct harness *h, const char *path)
{
    FILE *f;
    long sz;
    uint8_t *buf;
    bool ok;

    f = fopen(path, "rb");
    if (!f) { perror(path); return -1; }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    sz = ftell(f);
    if (sz <= 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }
    buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) { fclose(f); return -1; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf); fclose(f); return -1;
    }
    fclose(f);
    ok = cart_load(&h->cart, buf, (size_t)sz);
    free(buf);
    if (!ok) { fprintf(stderr, "cart_load failed (bad size?)\n"); return -1; }
    return 0;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
        "Usage: %s --rom ROM --halfclocks N --out FILE [--every K]\n"
        "  Produces an NDJSON trace of our tia.c matching run_sim2600.py.\n"
        "  --every K: emit one record every K half-clocks (default 2 = 1/pixel).\n",
        argv0);
}

int main(int argc, char **argv)
{
    const char *rom_path = NULL, *out_path = NULL;
    uint64_t hc = 0, every = 2;
    struct harness h;
    struct cpu_bus bus_cb;
    const char *base;
    int i;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--rom") && i + 1 < argc)
            rom_path = argv[++i];
        else if (!strcmp(argv[i], "--halfclocks") && i + 1 < argc)
            hc = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--out") && i + 1 < argc)
            out_path = argv[++i];
        else if (!strcmp(argv[i], "--every") && i + 1 < argc)
            every = strtoull(argv[++i], NULL, 10);
        else { usage(argv[0]); return 2; }
    }
    if (!rom_path || !out_path || !hc || !every) { usage(argv[0]); return 2; }

    memset(&h, 0, sizeof h);
    h.target_hc = hc;
    h.every = every;

    tia_init(&h.tia);
    riot_init(&h.riot);
    if (load_rom(&h, rom_path) < 0) return 1;

    bus_cb.read  = h_bus_read;
    bus_cb.write = h_bus_write;
    bus_cb.ctx   = &h;
    cpu_init(&h.cpu, bus_cb);

    h.trace = fopen(out_path, "w");
    if (!h.trace) { perror(out_path); return 1; }

    base = strrchr(rom_path, '/');
    base = base ? base + 1 : rom_path;
    fprintf(h.trace,
        "{\"format\": \"sim2600-trace\", \"version\": 1, "
        "\"source\": \"tia.c\", \"rom\": \"%s\", "
        "\"halfclocks\": %llu, \"every\": %llu}\n",
        base, (unsigned long long)hc, (unsigned long long)every);

    cpu_reset(&h.cpu);
    while (h.hc < h.target_hc)
        cpu_step(&h.cpu);

    fclose(h.trace);
    return 0;
}
