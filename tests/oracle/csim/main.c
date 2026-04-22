// Prototype driver: runs N half-clocks of the console sim and can emit
// (a) a per-half-clock wire-state trace for byte-level comparison against
// the Python oracle, (b) a raw RGBA pixel stream, or (c) a throughput
// benchmark.

#include "sim2600.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void usage(const char* argv0) {
    fprintf(stderr,
        "usage: %s --cpu CPU.ckt --tia TIA.ckt --rom ROM.bin\n"
        "           [--halfclocks N] [--trace FILE] [--pixels FILE]\n"
        "           [--ndjson FILE] [--every K] [--vtrace FILE]\n"
        "           [--bench] [--stats FILE]\n"
        "\n"
        "  --ndjson FILE  Emit an NDJSON oracle trace compatible with\n"
        "                 tests/tools/run_sim2600.py.  Header line, then one\n"
        "                 record per sample point.  Default cadence is every\n"
        "                 2 half-clocks (1 record per pixel).\n"
        "  --every K      Override sample cadence in half-clocks (K>=1).\n",
        argv0);
    exit(2);
}

// NDJSON emission helpers ---------------------------------------------------

static const char* basename_of(const char* path) {
    const char* s = strrchr(path, '/');
    return s ? s + 1 : path;
}

static void ndjson_header(FILE* f, const char* romPath, long halfclocks,
                          long every) {
    fprintf(f,
        "{\"format\": \"sim2600-trace\", \"version\": 1, "
        "\"rom\": \"%s\", \"halfclocks\": %ld, \"every\": %ld}\n",
        basename_of(romPath), halfclocks, every);
}

static void ndjson_record(FILE* f, const Console* c) {
    fprintf(f,
        "{\"hc\": %u, \"cpu_hc\": %u, \"addr\": %u, \"data\": %u, "
        "\"rw\": %d, \"sync\": %d, \"rdy\": %d, "
        "\"vsync\": %d, \"vblank\": %d, \"wsync\": %d, \"rsync\": %d, "
        "\"lum\": %d, \"col\": %d, \"rgba\": %u}\n",
        c->tia.halfClkCount,
        c->cpu.halfClkCount,
        cons_cpu_addr(c),
        cons_cpu_data(c),
        cons_cpu_rw_high(c)    ? 1 : 0,
        cons_cpu_sync_high(c)  ? 1 : 0,
        cons_cpu_rdy_high(c)   ? 1 : 0,
        cons_tia_vsync_high(c) ? 1 : 0,
        cons_tia_vblank_high(c)? 1 : 0,
        cons_tia_wsync_high(c) ? 1 : 0,
        cons_tia_rsync_high(c) ? 1 : 0,
        cons_tia_lum3(c),
        cons_tia_col4(c),
        cons_pixel_rgba8(c));
}

extern int cs_debug_flood;
static Console* s_vtraceCons;
static FILE*    s_vtraceFile;
static int      s_vstep;
static int      s_debug_vstep = -1;
static void dump_wire_states(FILE* f, const CircuitSim* cs);
static void vtrace_hook(const CircuitSim* cs, void* ud) {
    (void)ud;
    if (s_vstep == s_debug_vstep) cs_debug_flood = 0;  // end of target vstep
    if (!s_vtraceFile) return;
    const char* which = (cs == &s_vtraceCons->cpu) ? "CPU" : "TIA";
    fprintf(stderr, "vstep %d: %s recalc done, totalRecalc=%llu\n",
            s_vstep++, which, (unsigned long long)cs->numWiresRecalculated);
    if (s_vstep == s_debug_vstep) cs_debug_flood = 1;  // next one is target
    dump_wire_states(s_vtraceFile, &s_vtraceCons->cpu);
    dump_wire_states(s_vtraceFile, &s_vtraceCons->tia);
}

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void dump_wire_states(FILE* f, const CircuitSim* cs) {
    // One byte per wire — the same numeric state values the Python oracle
    // stores in wire.state.
    for (uint32_t i = 0; i < cs->numWires; i++) {
        uint8_t s = cs->wires[i].state;
        fwrite(&s, 1, 1, f);
    }
    // Transistor on/off, one byte each.
    fwrite(cs->transOn, 1, cs->numTrans, f);
}

int main(int argc, char** argv) {
    const char* cpuPath = NULL;
    const char* tiaPath = NULL;
    const char* romPath = NULL;
    const char* tracePath = NULL;
    const char* pixelsPath = NULL;
    const char* statsPath = NULL;
    const char* ndjsonPath = NULL;
    long halfClocks = 10000;
    long every = 2;
    int bench = 0;

    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "--cpu")        && i+1 < argc) cpuPath = argv[++i];
        else if (!strcmp(argv[i], "--tia")        && i+1 < argc) tiaPath = argv[++i];
        else if (!strcmp(argv[i], "--rom")        && i+1 < argc) romPath = argv[++i];
        else if (!strcmp(argv[i], "--halfclocks") && i+1 < argc) halfClocks = strtol(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--trace")      && i+1 < argc) tracePath = argv[++i];
        else if (!strcmp(argv[i], "--pixels")     && i+1 < argc) pixelsPath = argv[++i];
        else if (!strcmp(argv[i], "--vtrace")     && i+1 < argc) { s_vtraceFile = fopen(argv[++i], "wb"); if (!s_vtraceFile) { perror(argv[i]); return 1; } }
        else if (!strcmp(argv[i], "--debug-vstep") && i+1 < argc) s_debug_vstep = (int)strtol(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--bench"))                    bench = 1;
        else if (!strcmp(argv[i], "--stats")      && i+1 < argc) statsPath = argv[++i];
        else if (!strcmp(argv[i], "--ndjson")     && i+1 < argc) ndjsonPath = argv[++i];
        else if (!strcmp(argv[i], "--every")      && i+1 < argc) every = strtol(argv[++i], NULL, 0);
        else                                                     usage(argv[0]);
    }
    if (!cpuPath || !tiaPath || !romPath) usage(argv[0]);
    if (every < 1) { fprintf(stderr, "--every must be >= 1\n"); return 2; }

    Console c;
    s_vtraceCons = &c;
    if (getenv("SIM_INIT_PROBE")) cs_set_init_probe(1);
    if (cons_init(&c, cpuPath, tiaPath, romPath) != 0) {
        fprintf(stderr, "cons_init failed\n");
        return 1;
    }
    cs_set_init_probe(0);
    // Install vtrace hook only after init, so we don't dump during the
    // console's boot sequence (the Python side likewise starts recording
    // after Sim2600Console construction).
    if (s_vtraceFile) cs_set_recalc_hook(vtrace_hook, NULL);
    if (s_debug_vstep == 0) cs_debug_flood = 1;

    FILE* traceF  = tracePath  ? fopen(tracePath,  "wb") : NULL;
    FILE* pixelsF = pixelsPath ? fopen(pixelsPath, "wb") : NULL;
    FILE* statsF  = statsPath  ? fopen(statsPath,  "w")  : NULL;
    FILE* ndjsonF = ndjsonPath ? fopen(ndjsonPath, "w")  : NULL;
    if (ndjsonF) ndjson_header(ndjsonF, romPath, halfClocks, every);
    if ((ndjsonPath && !ndjsonF)) { perror("open ndjson"); return 1; }
    if (statsF) {
        cs_set_stats(1);
        fprintf(statsF, "# hc,cpu_recalcs,tia_recalcs,cpu_floods,cpu_wirewrites,"
                "cpu_maxgrp,tia_floods,tia_wirewrites,tia_maxgrp,"
                "cpu_toggles,tia_toggles\n");
    }
    if ((tracePath && !traceF) || (pixelsPath && !pixelsF)) {
        perror("open output");
        return 1;
    }

    // Before any half-clocks advance, record the post-init state.  The
    // Python reference dumper does the same, so byte-comparison lines up.
    if (traceF) {
        dump_wire_states(traceF, &c.cpu);
        dump_wire_states(traceF, &c.tia);
    }

    double t0 = now_sec();
    uint64_t wiresBefore = c.cpu.numWiresRecalculated + c.tia.numWiresRecalculated;

    uint64_t prevCpuRecalcs = 0, prevTiaRecalcs = 0;
    uint64_t prevCpuFloods = 0, prevTiaFloods = 0;
    uint64_t prevCpuWrites = 0, prevTiaWrites = 0;
    uint64_t prevCpuToggles = 0, prevTiaToggles = 0;
    uint32_t prevCpuMaxGrp = 0, prevTiaMaxGrp = 0;

    for (long i = 0; i < halfClocks; i++) {
        cons_advance_one_half_clock(&c);
        if (statsF) {
            uint64_t cpuR  = c.cpu.numWiresRecalculated;
            uint64_t tiaR  = c.tia.numWiresRecalculated;
            uint64_t cpuF  = c.cpu.statGroupSizeCount;
            uint64_t tiaF  = c.tia.statGroupSizeCount;
            uint64_t cpuW  = c.cpu.statWireWrites;
            uint64_t tiaW  = c.tia.statWireWrites;
            uint64_t cpuT  = c.cpu.statTransToggles;
            uint64_t tiaT  = c.tia.statTransToggles;
            uint32_t cpuMG = c.cpu.statMaxGroupSize;
            uint32_t tiaMG = c.tia.statMaxGroupSize;
            fprintf(statsF, "%ld,%llu,%llu,%llu,%llu,%u,%llu,%llu,%u,%llu,%llu\n",
                i,
                (unsigned long long)(cpuR - prevCpuRecalcs),
                (unsigned long long)(tiaR - prevTiaRecalcs),
                (unsigned long long)(cpuF - prevCpuFloods),
                (unsigned long long)(cpuW - prevCpuWrites),
                cpuMG - prevCpuMaxGrp,
                (unsigned long long)(tiaF - prevTiaFloods),
                (unsigned long long)(tiaW - prevTiaWrites),
                tiaMG - prevTiaMaxGrp,
                (unsigned long long)(cpuT - prevCpuToggles),
                (unsigned long long)(tiaT - prevTiaToggles));
            prevCpuRecalcs = cpuR; prevTiaRecalcs = tiaR;
            prevCpuFloods = cpuF; prevTiaFloods = tiaF;
            prevCpuWrites = cpuW; prevTiaWrites = tiaW;
            prevCpuToggles = cpuT; prevTiaToggles = tiaT;
            // max-group is sticky so we don't delta those; keep total
            prevCpuMaxGrp = 0; prevTiaMaxGrp = 0;
        }

        if (pixelsF && cons_tia_clk0_low(&c)) {
            uint32_t rgba = cons_pixel_rgba8(&c);
            uint8_t bytes[4] = {
                (uint8_t)(rgba >> 24),
                (uint8_t)(rgba >> 16),
                (uint8_t)(rgba >> 8),
                (uint8_t)(rgba & 0xFF),
            };
            fwrite(bytes, 1, 4, pixelsF);
        }
        if (ndjsonF && (c.tia.halfClkCount % (uint32_t)every) == 0) {
            ndjson_record(ndjsonF, &c);
        }
        if (traceF) {
            dump_wire_states(traceF, &c.cpu);
            dump_wire_states(traceF, &c.tia);
        }
    }

    double elapsed = now_sec() - t0;
    uint64_t wiresAfter = c.cpu.numWiresRecalculated + c.tia.numWiresRecalculated;

    if (bench) {
        double hcPerSec = halfClocks / elapsed;
        double realtime = 7159090.9 / hcPerSec;  // NTSC TIA clock ÷ 2 — half-clocks
        fprintf(stderr,
            "bench: %ld half-clocks in %.3f s  =  %.0f hc/s  "
            "(%.2f%% of realtime, %.2f wires/hc)\n",
            halfClocks, elapsed, hcPerSec, 100.0 / realtime,
            (double)(wiresAfter - wiresBefore) / halfClocks);
    }

    if (traceF)       fclose(traceF);
    if (pixelsF)      fclose(pixelsF);
    if (statsF)       fclose(statsF);
    if (ndjsonF)      fclose(ndjsonF);
    if (s_vtraceFile) fclose(s_vtraceFile);
    cons_free(&c);
    return 0;
}
