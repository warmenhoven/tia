// Port of sim2600Console.py + Sim6502.resetChip + SimTIA init sequence.

#include "sim2600.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// TIA color LUT.  Matches initColLumLUT() in simTIA.py.
// ---------------------------------------------------------------------------

static void init_color_lut(uint32_t lut[128]) {
    static const int base[16][2][3] = {
        {{0,0,0},       {236,236,236}},
        {{68,68,0},     {252,252,104}},
        {{112,40,0},    {236,200,120}},
        {{132,24,0},    {252,188,148}},
        {{136,0,0},     {252,180,180}},
        {{120,0,92},    {236,176,224}},
        {{72,0,120},    {212,176,252}},
        {{20,0,132},    {188,180,252}},
        {{0,0,136},     {164,164,252}},
        {{0,24,124},    {164,200,252}},
        {{0,44,92},     {164,224,252}},
        {{0,60,44},     {164,252,212}},
        {{0,60,0},      {184,252,184}},
        {{20,56,0},     {200,252,164}},
        {{44,48,0},     {224,236,156}},
        {{68,40,0},     {252,224,140}},
    };
    for (int c = 0; c < 16; c++) {
        for (int l = 0; l < 8; l++) {
            double frac = l / 7.0;
            int r = (int)(base[c][0][0] + (base[c][1][0] - base[c][0][0]) * frac);
            int g = (int)(base[c][0][1] + (base[c][1][1] - base[c][0][1]) * frac);
            int b = (int)(base[c][0][2] + (base[c][1][2] - base[c][0][2]) * frac);
            lut[(c << 3) | l] = ((uint32_t)r << 24) | ((uint32_t)g << 16) |
                                ((uint32_t)b << 8)  | 0xFF;
        }
    }
}

// ---------------------------------------------------------------------------
// Pad-index caching
// ---------------------------------------------------------------------------

static int16_t lookup(const CircuitSim* cs, const char* name) {
    int32_t i = cs_named_wire(cs, name);
    if (i < 0) {
        fprintf(stderr, "missing named pad: %s\n", name);
        exit(1);
    }
    return (int16_t)i;
}

static void cache_cpu_pads(Console* c) {
    static const char* abNames[16] = {
        "AB0","AB1","AB2","AB3","AB4","AB5","AB6","AB7",
        "AB8","AB9","AB10","AB11","AB12","AB13","AB14","AB15",
    };
    static const char* dbNames[8] = {
        "DB0","DB1","DB2","DB3","DB4","DB5","DB6","DB7",
    };
    for (int i = 0; i < 16; i++) c->cpuAB[i] = lookup(&c->cpu, abNames[i]);
    for (int i = 0; i < 8;  i++) c->cpuDB[i] = lookup(&c->cpu, dbNames[i]);
    c->cpuRW      = lookup(&c->cpu, "R/W");
    c->cpuCLK0    = lookup(&c->cpu, "CLK0");
    c->cpuRDY     = lookup(&c->cpu, "RDY");
    c->cpuCLK1OUT = lookup(&c->cpu, "CLK1OUT");
    c->cpuSYNC    = lookup(&c->cpu, "SYNC");
    c->cpuRES     = lookup(&c->cpu, "RES");
    c->cpuIRQ     = lookup(&c->cpu, "IRQ");
    c->cpuNMI     = lookup(&c->cpu, "NMI");
}

static void cache_tia_pads(Console* c) {
    static const char* abNames[6] = {"AB0","AB1","AB2","AB3","AB4","AB5"};
    static const char* dbNames[8] = {"DB0","DB1","DB2","DB3","DB4","DB5","DB6","DB7"};
    static const char* inNames[6] = {"I0","I1","I2","I3","I4","I5"};
    // Note: the Python file lists the TIA drivers as:
    //   ['DB6_drvLo', 'DB6_drvHi', 'DB7_drvHi', 'DB7_drvHi']
    // with DB7_drvLo duplicated as DB7_drvHi — a typo in the oracle.
    // We replicate the set the Python code actually consults.
    static const char* drvNames[4] = {"DB6_drvLo","DB6_drvHi","DB7_drvHi","DB7_drvHi"};

    for (int i = 0; i < 6; i++) c->tiaAB[i]    = lookup(&c->tia, abNames[i]);
    for (int i = 0; i < 8; i++) c->tiaDB[i]    = lookup(&c->tia, dbNames[i]);
    for (int i = 0; i < 6; i++) c->tiaInput[i] = lookup(&c->tia, inNames[i]);
    for (int i = 0; i < 4; i++) c->tiaDBDrivers[i] = lookup(&c->tia, drvNames[i]);

    c->tiaDB6drvLo   = lookup(&c->tia, "DB6_drvLo");
    c->tiaDB6drvHi   = lookup(&c->tia, "DB6_drvHi");
    c->tiaDB7drvLo   = lookup(&c->tia, "DB7_drvLo");
    c->tiaDB7drvHi   = lookup(&c->tia, "DB7_drvHi");
    c->tiaCLK0       = lookup(&c->tia, "CLK0");
    c->tiaCLK2       = lookup(&c->tia, "CLK2");
    c->tiaPH0        = lookup(&c->tia, "PH0");
    c->tiaCS0        = lookup(&c->tia, "CS0");
    c->tiaCS1        = lookup(&c->tia, "CS1");
    c->tiaCS2        = lookup(&c->tia, "CS2");
    c->tiaCS3        = lookup(&c->tia, "CS3");
    c->tiaRW         = lookup(&c->tia, "R/W");
    c->tiaDEL        = lookup(&c->tia, "del");
    c->tiaRDYlowCtrl = lookup(&c->tia, "RDY_lowCtrl");
    c->tiaVBLANK     = lookup(&c->tia, "VBLANK");
    c->tiaVSYNC      = lookup(&c->tia, "VSYNC");
    c->tiaWSYNC      = lookup(&c->tia, "WSYNC");
    c->tiaRSYNC      = lookup(&c->tia, "RSYNC");
    c->tiaL0         = lookup(&c->tia, "L0_lowCtrl");
    c->tiaL1         = lookup(&c->tia, "L1_lowCtrl");
    c->tiaL2         = lookup(&c->tia, "L2_lowCtrl");
    c->tiaCOLCNT[0]  = lookup(&c->tia, "COLCNT_T0");
    c->tiaCOLCNT[1]  = lookup(&c->tia, "COLCNT_T1");
    c->tiaCOLCNT[2]  = lookup(&c->tia, "COLCNT_T2");
    c->tiaCOLCNT[3]  = lookup(&c->tia, "COLCNT_T3");
}

// ---------------------------------------------------------------------------
// Memory I/O
// ---------------------------------------------------------------------------

static uint16_t cpu_get_addr(const Console* c) {
    uint16_t a = 0;
    for (int i = 0; i < 16; i++)
        if (cs_is_high(&c->cpu, c->cpuAB[i])) a |= (uint16_t)(1u << i);
    return a;
}
uint16_t cons_cpu_addr(const Console* c) { return cpu_get_addr(c); }

static uint8_t cpu_get_data(const Console* c) {
    uint8_t v = 0;
    for (int i = 0; i < 8; i++)
        if (cs_is_high(&c->cpu, c->cpuDB[i])) v |= (uint8_t)(1u << i);
    return v;
}
uint8_t cons_cpu_data(const Console* c) { return cpu_get_data(c); }

static void cpu_set_data(Console* c, uint8_t v) {
    for (int i = 0; i < 8; i++) {
        if (v & (1u << i)) cs_set_pulled_high(&c->cpu, c->cpuDB[i]);
        else               cs_set_pulled_low(&c->cpu, c->cpuDB[i]);
    }
}

// Forward decl — readMemory/writeMemory need each other via sequencing.
static void write_memory(Console* c, uint16_t addr, uint8_t value, bool setup);
static uint8_t read_memory(Console* c, uint16_t addr);

static uint8_t read_memory(Console* c, uint16_t addr) {
    if (addr > 0x02FF && addr < 0x8000) {
        fprintf(stderr, "ERROR: 6507 ROM read from 0x%X\n", addr);
        return 0;  // Python early-returns here too — no databus recalc.
    }

    uint8_t data = 0;
    if ((addr >= 0x80 && addr <= 0xFF) || (addr >= 0x180 && addr <= 0x1FF)) {
        data = c->piaRam[(addr & 0xFF) - 0x80];
    } else if (addr >= 0x0280 && addr <= 0x0297) {
        data = c->piaIot[addr - 0x0280];
    } else if (addr >= 0xF000 ||
               (addr >= 0xD000 && addr <= 0xDFFF && c->programLen == 8192)) {
        data = c->rom[addr - 0xF000 + c->bankOffset];
    } else if (addr >= 0x30 && addr <= 0x3D) {
        // TIA read: data comes from TIA's DB drivers below
    } else if (addr <= 0x2C || (addr >= 0x100 && addr <= 0x12C)) {
        // Curious: read from TIA write-only address.  Python no-op.
    } else {
        fprintf(stderr, "WARNING: Unhandled address in read_memory: 0x%X\n", addr);
    }

    if (cs_is_high(&c->cpu, c->cpuSYNC)) {
        // Sanity: TIA must not be driving DB during instruction fetch.
        for (int i = 0; i < 4; i++) {
            if (cs_is_high(&c->tia, c->tiaDBDrivers[i])) {
                fprintf(stderr, "ERROR: TIA driving DB during instruction fetch (addr 0x%X)\n", addr);
                break;
            }
        }
    } else {
        if (cs_is_high(&c->tia, c->tiaDB6drvLo)) data &= (uint8_t)~(1u << 6);
        if (cs_is_high(&c->tia, c->tiaDB6drvHi)) data |= (uint8_t)(1u << 6);
        if (cs_is_high(&c->tia, c->tiaDB7drvLo)) data &= (uint8_t)~(1u << 7);
        if (cs_is_high(&c->tia, c->tiaDB7drvHi)) data |= (uint8_t)(1u << 7);
    }

    cpu_set_data(c, data);
    cs_recalc_wires(&c->cpu, c->cpuDB, 8);
    return data;
}

static void write_memory(Console* c, uint16_t addr, uint8_t value, bool setup) {
    if (cs_is_low(&c->cpu, c->cpuRES) && !setup) return;

    if (addr >= 0xF000 && !setup) {
        if (c->programLen == 8192) {
            if      (addr == 0xFFF9) c->bankOffset = 0x2000;
            else if (addr == 0xFFF8) c->bankOffset = 0x1000;
        } else {
            fprintf(stderr, "ERROR: 6507 writing to ROM addr 0x%X data 0x%02X\n",
                    addr, value);
        }
    }

    if ((addr == 0x282 || addr == 0x280) && !setup) {
        fprintf(stderr, "ERROR: 6507 writing console switches 0x%X=0x%02X\n",
                addr, value);
        return;
    }

    if ((addr >= 0x80 && addr <= 0xFF) || (addr >= 0x180 && addr <= 0x1FF)) {
        c->piaRam[(addr & 0xFF) - 0x80] = value;
    } else if (addr >= 0x0280 && addr <= 0x0297) {
        c->piaIot[addr - 0x0280] = value;

        // Timer period set?  Python also assigns to a typo'd attribute
        // `pia.timerVal` (not `timerValue`) — dead-write, so skipped here.
        int32_t period = -1;
        switch (addr) {
            case 0x294: period = 1;    break;
            case 0x295: period = 8;    break;
            case 0x296: period = 64;   break;
            case 0x297: period = 1024; break;
        }
        if (period > 0) {
            c->piaTimerPeriod = period;
            c->piaTimerClockCount = 0;
            c->piaTimerFinished = false;
        }
    }
    // TIA writes (addr <= 0x2C) reach the TIA chip via data/address/RW pads,
    // not through this memory table.
}

// ---------------------------------------------------------------------------
// Data bus coordination
// ---------------------------------------------------------------------------

static void update_data_bus(Console* c) {
    for (int i = 0; i < 8; i++) {
        bool hi = cs_is_high(&c->cpu, c->cpuDB[i]);
        cs_set_pulled(&c->tia, c->tiaDB[i], hi);
    }
    cs_recalc_wires(&c->tia, c->tiaDB, 8);

    bool hidrv = false;
    for (int i = 0; i < 4; i++) {
        if (cs_is_high(&c->tia, c->tiaDBDrivers[i])) { hidrv = true; break; }
    }
    if (hidrv && cs_is_high(&c->cpu, c->cpuSYNC)) {
        fprintf(stderr, "ERROR: TIA driving DB during instruction fetch\n");
    }
}

// ---------------------------------------------------------------------------
// CPU reset — port of Sim6502.resetChip
// ---------------------------------------------------------------------------

static void reset_cpu(Console* c) {
    cs_recalc_all(&c->cpu);

    // setLowWN('RES') ; setHighWN('IRQ','NMI','RDY')
    cs_set_pulled_low (&c->cpu, c->cpuRES);
    cs_set_pulled_high(&c->cpu, c->cpuIRQ);
    cs_set_pulled_high(&c->cpu, c->cpuNMI);
    cs_set_pulled_high(&c->cpu, c->cpuRDY);
    uint16_t list[] = { c->cpuIRQ, c->cpuNMI, c->cpuRES, c->cpuRDY };
    cs_recalc_wires(&c->cpu, list, 4);

    // 4 half-clocks of CLK0 toggle
    for (int i = 0; i < 4; i++) {
        if (i & 1) cs_set_pulled_low (&c->cpu, c->cpuCLK0);
        else       cs_set_pulled_high(&c->cpu, c->cpuCLK0);
        cs_recalc_wire(&c->cpu, c->cpuCLK0);
    }

    cs_set_pulled_high(&c->cpu, c->cpuRES);
    cs_recalc_wire(&c->cpu, c->cpuRES);
}

// ---------------------------------------------------------------------------
// advanceOneHalfClock
// ---------------------------------------------------------------------------

void cons_advance_one_half_clock(Console* c) {
    CircuitSim* cpu = &c->cpu;
    CircuitSim* tia = &c->tia;

    if (tia->halfClkCount < 10) {
        for (int i = 0; i < 6; i++) cs_set_pulled_high(tia, c->tiaInput[i]);
        cs_recalc_wires(tia, c->tiaInput, 6);
    }

    cs_set_pulled_high(tia, c->tiaDEL);
    cs_recalc_wire(tia, c->tiaDEL);

    cs_set_pulled(tia, c->tiaRW, cs_is_high(cpu, c->cpuRW));
    cs_recalc_wire(tia, c->tiaRW);

    uint16_t addr = cpu_get_addr(c);

    // 6 TIA address-bus pins driven from CPU AB0..AB5.  Note: Python sets
    // the pad via setHigh/setLow (pulled+state together), not setPulled.
    for (int i = 0; i < 6; i++) {
        if (cs_is_high(cpu, c->cpuAB[i])) cs_set_pulled_high(tia, c->tiaAB[i]);
        else                              cs_set_pulled_low (tia, c->tiaAB[i]);
    }
    cs_recalc_wires(tia, c->tiaAB, 6);

    if (addr > 0x7F) {
        cs_set_pulled_high(tia, c->tiaCS3);
        cs_set_pulled_high(tia, c->tiaCS0);
    } else {
        cs_set_pulled_low(tia, c->tiaCS3);
        cs_set_pulled_low(tia, c->tiaCS0);
    }
    uint16_t csPair[2] = { c->tiaCS0, c->tiaCS3 };
    cs_recalc_wires(tia, csPair, 2);

    update_data_bus(c);

    cs_set_pulled(tia, c->tiaCLK2, cs_is_high(cpu, c->cpuCLK1OUT));
    cs_recalc_wire(tia, c->tiaCLK2);

    // Toggle TIA CLK0.
    cs_set_pulled(tia, c->tiaCLK0, !cs_is_high(tia, c->tiaCLK0));
    cs_recalc_wire(tia, c->tiaCLK0);
    tia->halfClkCount++;

    // TIA drives CPU RDY via RDY_lowCtrl.
    cs_set_pulled(cpu, c->cpuRDY, !cs_is_high(tia, c->tiaRDYlowCtrl));
    cs_recalc_wire(cpu, c->cpuRDY);

    bool clkTo6507High = cs_is_high(tia, c->tiaPH0);

    if (clkTo6507High != cs_is_high(cpu, c->cpuCLK0)) {
        if (clkTo6507High) {
            if (c->piaTimerFinished) {
                c->piaTimerValue--;
                if (c->piaTimerValue < 0) c->piaTimerValue = 0;
            } else {
                c->piaTimerClockCount++;
                if (c->piaTimerClockCount >= c->piaTimerPeriod) {
                    c->piaTimerValue--;
                    c->piaTimerClockCount = 0;
                    if (c->piaTimerValue < 0) {
                        c->piaTimerFinished = true;
                        c->piaTimerValue = 0xFF;
                    }
                }
            }
        }

        if (clkTo6507High) {
            cs_set_pulled_high(cpu, c->cpuCLK0);
        } else {
            cs_set_pulled_low(cpu, c->cpuCLK0);
            // Publish PIA timer to the memory-mapped I/O register.
            write_memory(c, 0x0284, (uint8_t)c->piaTimerValue, false);
        }
        cs_recalc_wire(cpu, c->cpuCLK0);
        cpu->halfClkCount++;

        uint16_t a2 = cpu_get_addr(c);
        if (cs_is_high(cpu, c->cpuCLK0)) {
            if (cs_is_low(cpu, c->cpuRW)) {
                write_memory(c, a2, cpu_get_data(c), false);
            }
        } else {
            if (cs_is_high(cpu, c->cpuRW)) {
                read_memory(c, a2);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Pixel output
// ---------------------------------------------------------------------------

bool cons_tia_clk0_low  (const Console* c) { return cs_is_low (&c->tia, c->tiaCLK0); }
bool cons_tia_vsync_high(const Console* c) { return cs_is_high(&c->tia, c->tiaVSYNC); }
bool cons_tia_vblank_high(const Console* c) { return cs_is_high(&c->tia, c->tiaVBLANK); }
bool cons_tia_wsync_high(const Console* c) { return cs_is_high(&c->tia, c->tiaWSYNC); }
bool cons_tia_rsync_high(const Console* c) { return cs_is_high(&c->tia, c->tiaRSYNC); }
bool cons_cpu_rw_high   (const Console* c) { return cs_is_high(&c->cpu, c->cpuRW); }
bool cons_cpu_sync_high (const Console* c) { return cs_is_high(&c->cpu, c->cpuSYNC); }
bool cons_cpu_rdy_high  (const Console* c) { return cs_is_high(&c->cpu, c->cpuRDY); }

int cons_tia_lum3(const Console* c) {
    int lum = 7;
    if (cs_is_high(&c->tia, c->tiaL0)) lum -= 1;
    if (cs_is_high(&c->tia, c->tiaL1)) lum -= 2;
    if (cs_is_high(&c->tia, c->tiaL2)) lum -= 4;
    return lum;
}

int cons_tia_col4(const Console* c) {
    int col = 0;
    if (cs_is_high(&c->tia, c->tiaCOLCNT[0])) col |= 1;
    if (cs_is_high(&c->tia, c->tiaCOLCNT[1])) col |= 2;
    if (cs_is_high(&c->tia, c->tiaCOLCNT[2])) col |= 4;
    if (cs_is_high(&c->tia, c->tiaCOLCNT[3])) col |= 8;
    return col;
}

uint32_t cons_pixel_rgba8(const Console* c) {
    int lum = cons_tia_lum3(c);
    int col = cons_tia_col4(c);
    return c->colLumLUT[((col & 0xF) << 3) | (lum & 7)];
}

// ---------------------------------------------------------------------------
// Init / deinit — mirrors Sim2600Console.__init__ exactly
// ---------------------------------------------------------------------------

static int load_rom(Console* c, const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) { perror(path); return -1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz != 2048 && sz != 4096 && sz != 8192) {
        fprintf(stderr, "%s: unsupported ROM size %ld\n", path, sz);
        fclose(f);
        return -1;
    }

    uint8_t tmp[8192];
    if (fread(tmp, 1, (size_t)sz, f) != (size_t)sz) { perror(path); fclose(f); return -1; }
    fclose(f);

    c->programLen = (size_t)sz;
    if (sz == 2048) {
        memcpy(c->rom,        tmp, 2048);
        memcpy(c->rom + 2048, tmp, 2048);
    } else if (sz == 4096) {
        memcpy(c->rom, tmp, 4096);
    } else {
        memcpy(c->rom, tmp, 8192);
        c->bankOffset = 0x1000;
    }

    // Match loadProgramBytes(setResetVector=False): read vector bytes so the
    // CPU data-bus pad wires end up in the same state Python leaves them in.
    (void)read_memory(c, 0xFFFA);
    (void)read_memory(c, 0xFFFB);
    (void)read_memory(c, 0xFFFC);
    (void)read_memory(c, 0xFFFD);
    (void)read_memory(c, 0xFFFE);
    (void)read_memory(c, 0xFFFF);
    return 0;
}

int cons_init(Console* c, const char* cpuCktPath, const char* tiaCktPath,
              const char* romPath) {
    memset(c, 0, sizeof(*c));
    init_color_lut(c->colLumLUT);

    if (cs_load(&c->cpu, cpuCktPath, "6502")) return -1;
    if (cs_load(&c->tia, tiaCktPath, "TIA"))  return -1;

    // Cache pad indices now that circuits are loaded.
    cache_cpu_pads(c);
    cache_tia_pads(c);

    // SimTIA.__init__ pulls CS3 and CS0 high (via setHighWN) before recalcAll.
    cs_set_pulled_high(&c->tia, c->tiaCS3);
    cs_set_pulled_high(&c->tia, c->tiaCS0);
    cs_recalc_all(&c->tia);

    // Console init: ROM + vector reads.
    if (load_rom(c, romPath)) return -1;

    // 6502 reset sequence.
    reset_cpu(c);

    // IRQ/NMI are tied high on 6507.
    cs_set_pulled_high(&c->cpu, c->cpuIRQ);
    cs_set_pulled_high(&c->cpu, c->cpuNMI);
    uint16_t cpuIrqNmi[2] = { c->cpuIRQ, c->cpuNMI };
    cs_recalc_wires(&c->cpu, cpuIrqNmi, 2);

    // TIA CS1 always high, CS2 always grounded.
    cs_set_pulled_high(&c->tia, c->tiaCS1);
    cs_set_pulled_low (&c->tia, c->tiaCS2);
    uint16_t tiaCs12[2] = { c->tiaCS1, c->tiaCS2 };
    cs_recalc_wires(&c->tia, tiaCs12, 2);

    // Console switches & joystick.
    write_memory(c, 0x0282, 0x0B, true);
    write_memory(c, 0x0280, 0xFF, true);

    return 0;
}

void cons_free(Console* c) {
    cs_free(&c->cpu);
    cs_free(&c->tia);
    memset(c, 0, sizeof(*c));
}
