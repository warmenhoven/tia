CC      ?= cc
CFLAGS  ?= -O3 -march=native -std=c11 -Wall -Wextra -Wno-unused-parameter
LDFLAGS ?=

OBJS = circuit.o console.o main.o
BIN  = sim2600

SIM_DIR := ../Sim2600

# `chip_*.ckt` are checked into the repo so csim builds standalone —
# regenerating them requires Sim2600's pickle files and is a deliberate
# `make chips` step, not part of the default build.
all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

%.o: %.c sim2600.h
	$(CC) $(CFLAGS) -c -o $@ $<

chips: chip_6502.ckt chip_TIA.ckt

chip_6502.ckt chip_TIA.ckt: convert.py $(SIM_DIR)/chips/net_6502.pkl $(SIM_DIR)/chips/net_TIA.pkl
	python3 convert.py

clean:
	rm -f $(OBJS) $(BIN)

distclean: clean
	rm -f chip_6502.ckt chip_TIA.ckt

.PHONY: all chips clean distclean
