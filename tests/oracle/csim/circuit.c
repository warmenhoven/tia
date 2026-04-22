// Port of circuitSimulatorUsingLists.py + circuitSimulatorBase.py.

#include "sim2600.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Debug: if set, print every wire added to a flood-group and every
// transistor toggle.  Enabled by main.c for one specific vstep.
int cs_debug_flood;

static int s_statsOn;

// ------- loading ------------------------------------------------------------

static int read_exact(void* dst, size_t n, FILE* f) {
    return fread(dst, 1, n, f) == n ? 0 : -1;
}

int cs_load(CircuitSim* cs, const char* path, const char* chipName) {
    memset(cs, 0, sizeof(*cs));
    cs->chipName = chipName;

    FILE* f = fopen(path, "rb");
    if (!f) { perror(path); return -1; }

    uint32_t hdr[9];  // magic (4B) + 8 * uint32
    if (read_exact(hdr, sizeof(hdr), f)) { fclose(f); return -1; }
    if (memcmp(&hdr[0], "CKT1", 4) != 0) {
        fprintf(stderr, "%s: bad magic\n", path);
        fclose(f);
        return -1;
    }

    cs->numWires     = hdr[1];
    cs->numTrans     = hdr[2];
    uint32_t ctLen   = hdr[3];
    uint32_t gtLen   = hdr[4];
    cs->vccIndex     = hdr[5];
    cs->gndIndex     = hdr[6];
    uint32_t numNamed = hdr[7];

    cs->wires    = calloc(cs->numWires, sizeof(Wire));
    cs->trans    = calloc(cs->numTrans, sizeof(Transistor));
    cs->transOn  = calloc(cs->numTrans, 1);
    cs->ctPool   = calloc(ctLen ? ctLen : 1, sizeof(uint16_t));
    cs->gatePool = calloc(gtLen ? gtLen : 1, sizeof(uint16_t));
    cs->named    = calloc(numNamed ? numNamed : 1, sizeof(NamedWire));
    cs->numNamed = numNamed;

    for (uint32_t i = 0; i < cs->numWires; i++) {
        struct __attribute__((packed)) { uint8_t p, _pad; uint16_t cc, gc, _pad2; uint32_t co, go; } rec;
        if (read_exact(&rec, sizeof(rec), f)) goto fail;
        cs->wires[i].pulled         = rec.p;
        cs->wires[i].state          = rec.p;
        cs->wires[i].ctCount        = rec.cc;
        cs->wires[i].gateCount      = rec.gc;
        cs->wires[i].ctOffset       = rec.co;
        cs->wires[i].gateOffset     = rec.go;
        cs->wires[i].lastGroupState = UINT32_MAX;
    }

    for (uint32_t i = 0; i < cs->numTrans; i++) {
        struct __attribute__((packed)) { uint16_t s1, s2, g, _pad; } rec;
        if (read_exact(&rec, sizeof(rec), f)) goto fail;
        cs->trans[i].s1 = rec.s1;
        cs->trans[i].s2 = rec.s2;
        cs->trans[i].gate = rec.g;
    }

    if (ctLen && read_exact(cs->ctPool, ctLen * sizeof(uint16_t), f)) goto fail;
    if (gtLen && read_exact(cs->gatePool, gtLen * sizeof(uint16_t), f)) goto fail;

    for (uint32_t i = 0; i < numNamed; i++) {
        uint16_t nameLen, idx;
        if (read_exact(&nameLen, 2, f)) goto fail;
        if (read_exact(&idx, 2, f))     goto fail;
        if (nameLen >= sizeof(cs->named[i].name)) {
            fprintf(stderr, "%s: pad name too long (%u)\n", path, nameLen);
            goto fail;
        }
        if (read_exact(cs->named[i].name, nameLen, f)) goto fail;
        cs->named[i].name[nameLen] = 0;
        cs->named[i].wireIndex = idx;
        if (nameLen & 1) {
            char pad;
            if (read_exact(&pad, 1, f)) goto fail;
        }
    }

    fclose(f);

    // Recalc machinery.  Queue capacity is a conservative upper bound:
    // each transistor toggle queues its two side wires, so 2 * numTrans
    // is plenty.  Flag array is keyed by wire index.
    uint32_t flagCap = cs->numWires > cs->numTrans ? cs->numWires : cs->numTrans;
    cs->queueCap        = 2 * cs->numTrans + cs->numWires;
    cs->recalcFlag      = calloc(flagCap, 1);
    cs->newRecalcFlag   = calloc(flagCap, 1);
    cs->recalcOrder     = calloc(cs->queueCap, sizeof(uint16_t));
    cs->newRecalcOrder  = calloc(cs->queueCap, sizeof(uint16_t));
    cs->groupList       = calloc(cs->numWires, sizeof(uint16_t));
    cs->dfsStack        = calloc(cs->numWires, sizeof(uint16_t));

    // VCC/VSS initial state; transistors gated by VCC start on.
    cs->wires[cs->vccIndex].state = WIRE_HIGH;
    cs->wires[cs->gndIndex].state = WIRE_GROUNDED;
    Wire* vcc = &cs->wires[cs->vccIndex];
    for (uint32_t i = 0; i < vcc->gateCount; i++) {
        cs->transOn[cs->gatePool[vcc->gateOffset + i]] = 1;
    }

    return 0;

fail:
    perror(path);
    fclose(f);
    return -1;
}

void cs_free(CircuitSim* cs) {
    free(cs->wires);
    free(cs->trans);
    free(cs->transOn);
    free(cs->ctPool);
    free(cs->gatePool);
    free(cs->named);
    free(cs->recalcFlag);
    free(cs->newRecalcFlag);
    free(cs->recalcOrder);
    free(cs->newRecalcOrder);
    free(cs->groupList);
    free(cs->dfsStack);
    memset(cs, 0, sizeof(*cs));
}

int32_t cs_named_wire(const CircuitSim* cs, const char* name) {
    for (uint32_t i = 0; i < cs->numNamed; i++) {
        if (strcmp(cs->named[i].name, name) == 0)
            return (int32_t)cs->named[i].wireIndex;
    }
    return -1;
}

// ------- core simulator -----------------------------------------------------

// Iterative DFS flood through currently-on transistors.  Uses groupList
// as both the output (visited-in-pre-order) and an auxiliary "pending
// expansion" stack.  Pushing children in reverse order preserves Python's
// recursive pre-order, which we need for byte-identical intermediate state.
//
// Implementation: we keep an extra stack `dfsStack` of wires whose
// expansion hasn't started yet.  When we pop one, we check whether it's
// already visited; if not, we add it to groupList, then push its
// on-neighbours in reverse order.  This matches `add self` then recurse
// into children left-to-right.
// Iterative DFS: matches Python's recursive pre-order.  Uses a dedicated
// stack (dfsStack) so the visited list (groupList) isn't disturbed.
static void flood_group(CircuitSim* cs, uint16_t seed) {
    cs->groupGen++;
    cs->groupCount = 0;
    cs->groupValue = 0;

    if (__builtin_expect(s_statsOn, 0)) cs->statGroupSizeCount++;

    const uint32_t vcc = cs->vccIndex;
    const uint32_t gnd = cs->gndIndex;
    const uint32_t gen = cs->groupGen;
    Wire* const wires = cs->wires;
    Transistor* const trans = cs->trans;
    const uint8_t* const transOn = cs->transOn;
    const uint16_t* const ctPool = cs->ctPool;
    uint16_t* const stack = cs->dfsStack;
    uint16_t* const groupList = cs->groupList;

    uint32_t sp = 0;
    stack[sp++] = seed;

    while (sp > 0) {
        uint16_t wi = stack[--sp];
        Wire* w = &wires[wi];
        if (w->lastGroupState == gen) continue;
        w->lastGroupState = gen;
        groupList[cs->groupCount++] = wi;

        if (wi == vcc) { cs->groupValue |= WIRE_HIGH;     continue; }
        if (wi == gnd) { cs->groupValue |= WIRE_GROUNDED; continue; }

        cs->groupValue |= w->pulled;
        if      (w->state == WIRE_FLOATING_LOW)  cs->groupValue |= WIRE_FLOATING_LOW;
        else if (w->state == WIRE_FLOATING_HIGH) cs->groupValue |= WIRE_FLOATING_HIGH;

        const uint16_t* ct = &ctPool[w->ctOffset];
        uint32_t n = w->ctCount;
        // Push children in REVERSE order so LIFO pop reproduces
        // Python's left-to-right recursion pre-order.
        for (uint32_t i = n; i--; ) {
            uint16_t ti = ct[i];
            if (!transOn[ti]) continue;
            Transistor* t = &trans[ti];
            uint16_t other = (t->s1 == wi) ? t->s2 : t->s1;
            if (wires[other].lastGroupState == gen) continue;
            stack[sp++] = other;
        }
    }
    if (__builtin_expect(s_statsOn, 0)) {
        cs->statGroupSizeSum += cs->groupCount;
        if (cs->groupCount > cs->statMaxGroupSize) cs->statMaxGroupSize = cs->groupCount;
    }
}

static uint8_t count_wire_sizes(const CircuitSim* cs) {
    uint32_t fl = 0, fh = 0;
    for (uint32_t i = 0; i < cs->groupCount; i++) {
        const Wire* w = &cs->wires[cs->groupList[i]];
        uint32_t num = w->ctCount + w->gateCount;
        if      (w->state == WIRE_FLOATING_LOW)  fl += num;
        else if (w->state == WIRE_FLOATING_HIGH) fh += num;
    }
    return (fh < fl) ? WIRE_FLOATING_LOW : WIRE_FLOATING_HIGH;
}

static inline void queue_recalc(CircuitSim* cs, uint16_t wi) {
    if (cs->newRecalcFlag[wi] == 0) {
        cs->newRecalcFlag[wi] = 1;
        cs->newRecalcOrder[cs->newRecalcCount++] = wi;
    }
}

static inline void float_wire(CircuitSim* cs, uint16_t wi) {
    Wire* w = &cs->wires[wi];
    if (w->pulled == WIRE_PULLED_HIGH) {
        w->state = WIRE_PULLED_HIGH;
    } else if (w->pulled == WIRE_PULLED_LOW) {
        w->state = WIRE_PULLED_LOW;
    } else {
        uint8_t s = w->state;
        if      (s == WIRE_GROUNDED || s == WIRE_PULLED_LOW)  w->state = WIRE_FLOATING_LOW;
        else if (s == WIRE_HIGH     || s == WIRE_PULLED_HIGH) w->state = WIRE_FLOATING_HIGH;
    }
}

static inline void turn_on(CircuitSim* cs, Transistor* t) {
    cs->transOn[t - cs->trans] = 1;
    if (__builtin_expect(cs_debug_flood, 0))
        fprintf(stderr, "  ON  t=%ld s1=%u s2=%u gate=%u\n",
                t - cs->trans, t->s1, t->s2, t->gate);
    queue_recalc(cs, t->s1);
    queue_recalc(cs, t->s2);
}

static inline void turn_off(CircuitSim* cs, Transistor* t) {
    cs->transOn[t - cs->trans] = 0;
    if (__builtin_expect(cs_debug_flood, 0))
        fprintf(stderr, "  OFF t=%ld s1=%u s2=%u gate=%u\n",
                t - cs->trans, t->s1, t->s2, t->gate);
    float_wire(cs, t->s1);
    float_wire(cs, t->s2);
    queue_recalc(cs, t->s1);
    queue_recalc(cs, t->s2);
}

static void do_wire_recalc(CircuitSim* cs, uint16_t wi) {
    if (wi == cs->vccIndex || wi == cs->gndIndex) return;

    if (__builtin_expect(cs_debug_flood, 0))
        fprintf(stderr, "== recalc wire %u\n", wi);
    flood_group(cs, wi);
    if (__builtin_expect(cs_debug_flood, 0))
        fprintf(stderr, "   groupValue=0x%X groupCount=%u\n",
                cs->groupValue, cs->groupCount);

    uint32_t gv = cs->groupValue;
    uint8_t newValue;
    if      (gv & WIRE_GROUNDED)    newValue = WIRE_GROUNDED;
    else if (gv & WIRE_HIGH)        newValue = WIRE_HIGH;
    else if (gv & WIRE_PULLED_LOW)  newValue = WIRE_PULLED_LOW;
    else if (gv & WIRE_PULLED_HIGH) newValue = WIRE_PULLED_HIGH;
    else if ((gv & WIRE_FLOATING_LOW) && (gv & WIRE_FLOATING_HIGH))
        newValue = count_wire_sizes(cs);
    else if (gv & WIRE_FLOATING_LOW)  newValue = WIRE_FLOATING_LOW;
    else if (gv & WIRE_FLOATING_HIGH) newValue = WIRE_FLOATING_HIGH;
    else                              newValue = cs->wires[cs->groupList[0]].state;

    bool newHigh = (newValue == WIRE_HIGH || newValue == WIRE_PULLED_HIGH ||
                    newValue == WIRE_FLOATING_HIGH);

    for (uint32_t i = 0; i < cs->groupCount; i++) {
        uint16_t wj = cs->groupList[i];
        if (wj == cs->vccIndex || wj == cs->gndIndex) continue;
        Wire* w = &cs->wires[wj];
        w->state = newValue;

        const uint16_t* gs = &cs->gatePool[w->gateOffset];
        uint32_t n = w->gateCount;
        if (__builtin_expect(s_statsOn, 0)) cs->statWireWrites++;
        if (newHigh) {
            for (uint32_t k = 0; k < n; k++) {
                uint16_t ti = gs[k];
                if (!cs->transOn[ti]) {
                    if (__builtin_expect(s_statsOn, 0)) cs->statTransToggles++;
                    turn_on(cs, &cs->trans[ti]);
                }
            }
        } else {
            for (uint32_t k = 0; k < n; k++) {
                uint16_t ti = gs[k];
                if (cs->transOn[ti]) {
                    if (__builtin_expect(s_statsOn, 0)) cs->statTransToggles++;
                    turn_off(cs, &cs->trans[ti]);
                }
            }
        }
    }
}

static CsRecalcHook s_recalcHook;
static void*        s_recalcUd;
void cs_set_recalc_hook(CsRecalcHook cb, void* ud) { s_recalcHook = cb; s_recalcUd = ud; }

// Init-phase probe: if non-NULL, fires after each recalc call with the
// simulator so callers can log count-deltas without needing the full
// state-dump hook machinery.  Logs to stderr.
static int s_initProbe;
void cs_set_init_probe(int on) { s_initProbe = on; }

void cs_set_stats(int on) { s_statsOn = on; }

static void do_recalc_iterations(CircuitSim* cs) {
    const int stepLimit = 400;
    int step = 0;
    if (__builtin_expect(s_statsOn, 0)) cs->statRecalcCallCount++;
    while (step < stepLimit && cs->recalcCount > 0) {
        for (uint32_t i = 0; i < cs->recalcCount; i++) {
            uint16_t wi = cs->recalcOrder[i];
            cs->newRecalcFlag[wi] = 0;
            do_wire_recalc(cs, wi);
            cs->recalcFlag[wi] = 0;
            cs->numWiresRecalculated++;
        }
        // Swap queues.
        uint8_t*  tf = cs->recalcFlag;   cs->recalcFlag   = cs->newRecalcFlag;   cs->newRecalcFlag  = tf;
        uint16_t* to = cs->recalcOrder;  cs->recalcOrder  = cs->newRecalcOrder;  cs->newRecalcOrder = to;
        cs->recalcCount    = cs->newRecalcCount;
        cs->newRecalcCount = 0;
        step++;
    }
    if (step >= stepLimit) {
        fprintf(stderr, "ERROR: sim '%s' did not converge after %d iterations\n",
                cs->chipName ? cs->chipName : "?", stepLimit);
        if (cs->halfClkCount > 0) abort();
    }

    // Matches circuitSimulatorBase.py: during the first 20 half-clocks, if
    // a non-converging recalc leaves stale flags in recalcArray, wipe them.
    // Without this, a later cs_recalc_wire/cs_recalc_wires would dedup
    // against a dirty flag and silently skip enqueueing a wire — produces
    // a 1-wire-off count vs the Python oracle.
    if (cs->halfClkCount < 20) {
        bool dirty = false;
        for (uint32_t i = 0; i < cs->numWires; i++) {
            if (cs->recalcFlag[i]) { dirty = true; break; }
        }
        if (dirty) memset(cs->recalcFlag, 0, cs->numWires);
    }
}

void cs_recalc_wire(CircuitSim* cs, uint16_t wi) {
    cs->recalcCount = 0;
    if (cs->recalcFlag[wi] == 0) {
        cs->recalcFlag[wi] = 1;
        cs->recalcOrder[cs->recalcCount++] = wi;
    }
    do_recalc_iterations(cs);
    if (s_initProbe) fprintf(stderr, "  probe %s count=%llu\n",
                             cs->chipName ? cs->chipName : "?",
                             (unsigned long long)cs->numWiresRecalculated);
    if (s_recalcHook) s_recalcHook(cs, s_recalcUd);
}

void cs_recalc_wires(CircuitSim* cs, const uint16_t* wires, uint32_t n) {
    cs->recalcCount = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint16_t w = wires[i];
        if (cs->recalcFlag[w] == 0) {
            cs->recalcFlag[w] = 1;
            cs->recalcOrder[cs->recalcCount++] = w;
        }
    }
    do_recalc_iterations(cs);
    if (s_initProbe) fprintf(stderr, "  probe %s count=%llu\n",
                             cs->chipName ? cs->chipName : "?",
                             (unsigned long long)cs->numWiresRecalculated);
    if (s_recalcHook) s_recalcHook(cs, s_recalcUd);
}

void cs_recalc_all(CircuitSim* cs) {
    cs->recalcCount = 0;
    for (uint32_t i = 0; i < cs->numWires; i++) {
        cs->recalcFlag[i] = 1;
        cs->recalcOrder[cs->recalcCount++] = (uint16_t)i;
    }
    do_recalc_iterations(cs);
    if (s_initProbe) fprintf(stderr, "  probe %s count=%llu\n",
                             cs->chipName ? cs->chipName : "?",
                             (unsigned long long)cs->numWiresRecalculated);
    if (s_recalcHook) s_recalcHook(cs, s_recalcUd);
}
