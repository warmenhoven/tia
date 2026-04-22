// Native port of Sim2600. Matches the semantics of circuitSimulatorUsingLists.py.

#ifndef SIM2600_H
#define SIM2600_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Wire state bit flags — values match wire.py exactly.
#define WIRE_PULLED_HIGH    0x01u
#define WIRE_PULLED_LOW     0x02u
#define WIRE_GROUNDED       0x04u
#define WIRE_HIGH           0x08u
#define WIRE_FLOATING_HIGH  0x10u
#define WIRE_FLOATING_LOW   0x20u
#define WIRE_FLOATING       0x40u

typedef struct {
    uint8_t  pulled;
    uint8_t  state;
    uint16_t ctCount;
    uint16_t gateCount;
    uint16_t _pad;
    uint32_t ctOffset;
    uint32_t gateOffset;
    uint32_t lastGroupState;
} Wire;

typedef struct {
    uint16_t s1;
    uint16_t s2;
    uint16_t gate;
    uint8_t  _pad0;
    uint8_t  _pad1;
} Transistor;

typedef struct {
    char     name[28];
    uint32_t wireIndex;
} NamedWire;

typedef struct {
    // Topology (static after load).
    Wire*       wires;
    uint32_t    numWires;
    Transistor* trans;
    uint8_t*    transOn;   // parallel to trans[] — hot-loop on/off flag
    uint32_t    numTrans;
    uint16_t*   ctPool;
    uint16_t*   gatePool;
    uint32_t    vccIndex;
    uint32_t    gndIndex;

    NamedWire*  named;
    uint32_t    numNamed;

    // Recalc queues — swapped by pointer each iteration.
    uint8_t*    recalcFlag;
    uint16_t*   recalcOrder;
    uint32_t    recalcCount;
    uint8_t*    newRecalcFlag;
    uint16_t*   newRecalcOrder;
    uint32_t    newRecalcCount;
    uint32_t    queueCap;

    // Group flood-fill scratch.
    uint16_t*   groupList;
    uint16_t*   dfsStack;
    uint32_t    groupCount;
    uint32_t    groupValue;
    uint32_t    groupGen;

    // Diagnostics.
    uint32_t    halfClkCount;
    uint64_t    numWiresRecalculated;

    // Per-recalc-call statistics (captured when cs_stats_enabled).
    uint64_t    statGroupSizeSum;     // sum of group sizes across flood_groups
    uint64_t    statGroupSizeCount;   // number of flood_groups
    uint32_t    statMaxGroupSize;
    uint64_t    statIterSum;          // sum of iteration counts across recalcs
    uint64_t    statRecalcCallCount;  // number of recalc calls
    uint32_t    statMaxIter;
    uint64_t    statWireWrites;       // count of wire state writes
    uint64_t    statTransToggles;     // count of transistor on/off toggles

    const char* chipName;
} CircuitSim;

int  cs_load(CircuitSim* cs, const char* path, const char* chipName);
void cs_free(CircuitSim* cs);
int32_t cs_named_wire(const CircuitSim* cs, const char* name);

void cs_recalc_all(CircuitSim* cs);
void cs_recalc_wire(CircuitSim* cs, uint16_t wireIndex);
void cs_recalc_wires(CircuitSim* cs, const uint16_t* wires, uint32_t count);

// Optional hook fired after each recalc call (for per-step tracing).
typedef void (*CsRecalcHook)(const CircuitSim* cs, void* userdata);
void cs_set_recalc_hook(CsRecalcHook cb, void* userdata);

void cs_set_init_probe(int on);
void cs_set_stats(int on);

static inline bool cs_is_high(const CircuitSim* cs, uint16_t i) {
    uint8_t s = cs->wires[i].state;
    return s == WIRE_HIGH || s == WIRE_PULLED_HIGH || s == WIRE_FLOATING_HIGH;
}
static inline bool cs_is_low(const CircuitSim* cs, uint16_t i) {
    uint8_t s = cs->wires[i].state;
    return s == WIRE_GROUNDED || s == WIRE_PULLED_LOW || s == WIRE_FLOATING_LOW;
}

static inline void cs_set_pulled_high(CircuitSim* cs, uint16_t i) {
    cs->wires[i].pulled = WIRE_PULLED_HIGH;
    cs->wires[i].state  = WIRE_PULLED_HIGH;
}
static inline void cs_set_pulled_low(CircuitSim* cs, uint16_t i) {
    cs->wires[i].pulled = WIRE_PULLED_LOW;
    cs->wires[i].state  = WIRE_PULLED_LOW;
}
static inline void cs_set_pulled(CircuitSim* cs, uint16_t i, bool high) {
    if (high) cs_set_pulled_high(cs, i);
    else      cs_set_pulled_low(cs, i);
}

// --- 2600 console layer ---

typedef struct {
    CircuitSim cpu;
    CircuitSim tia;

    // PIA emulation (matches emuPIA.py).
    uint8_t  piaRam[128];
    uint8_t  piaIot[24];
    int32_t  piaTimerPeriod;
    int32_t  piaTimerValue;
    int32_t  piaTimerClockCount;
    bool     piaTimerFinished;

    uint8_t  rom[8192];
    size_t   programLen;
    size_t   bankOffset;

    // TIA color LUT: 128 entries, (R<<24)|(G<<16)|(B<<8)|0xFF.
    uint32_t colLumLUT[128];

    // Cached pad indices.
    uint16_t cpuAB[16];
    uint16_t cpuDB[8];
    uint16_t cpuRW;
    uint16_t cpuCLK0;
    uint16_t cpuRDY;
    uint16_t cpuCLK1OUT;
    uint16_t cpuSYNC;
    uint16_t cpuRES;
    uint16_t cpuIRQ;
    uint16_t cpuNMI;

    uint16_t tiaAB[6];
    uint16_t tiaDB[8];
    uint16_t tiaInput[6];
    uint16_t tiaDBDrivers[4];
    uint16_t tiaDB6drvLo, tiaDB6drvHi, tiaDB7drvLo, tiaDB7drvHi;
    uint16_t tiaCLK0, tiaCLK2, tiaPH0;
    uint16_t tiaCS0, tiaCS1, tiaCS2, tiaCS3;
    uint16_t tiaRW, tiaDEL;
    uint16_t tiaRDYlowCtrl;
    uint16_t tiaVBLANK, tiaVSYNC, tiaWSYNC, tiaRSYNC;
    uint16_t tiaL0, tiaL1, tiaL2;
    uint16_t tiaCOLCNT[4];
} Console;

int  cons_init(Console* c, const char* cpuCktPath, const char* tiaCktPath,
               const char* romPath);
void cons_free(Console* c);

void cons_advance_one_half_clock(Console* c);
uint32_t cons_pixel_rgba8(const Console* c);   // valid when TIA CLK0 is low
bool cons_tia_clk0_low(const Console* c);
bool cons_tia_vsync_high(const Console* c);
bool cons_tia_vblank_high(const Console* c);

uint16_t cons_cpu_addr(const Console* c);
uint8_t  cons_cpu_data(const Console* c);

// Accessors for oracle NDJSON output — mirror fields the Python oracle
// emits in run_sim2600.py.
int  cons_tia_lum3     (const Console* c);   // 3-bit luminance driving DAC
int  cons_tia_col4     (const Console* c);   // 4-bit colour counter
bool cons_tia_wsync_high(const Console* c);
bool cons_tia_rsync_high(const Console* c);
bool cons_cpu_rw_high  (const Console* c);
bool cons_cpu_sync_high(const Console* c);
bool cons_cpu_rdy_high (const Console* c);

#endif // SIM2600_H
