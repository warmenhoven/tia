CC     ?= cc
CFLAGS ?= -O2 -Wall -Wextra -I../../src

SRC_DIR      := ../../src
CORE_SOURCES := $(SRC_DIR)/cpu.c $(SRC_DIR)/tia.c \
                $(SRC_DIR)/cart.c $(SRC_DIR)/riot.c

CSIM_DIR := ../oracle/csim

BINS := run_tia_c

.PHONY: all clean csim csim-clean
all: $(BINS)

$(BINS): %: %.c $(CORE_SOURCES)
	$(CC) $(CFLAGS) -o $@ $< $(CORE_SOURCES)

# Build the native Sim2600 C port. run_sim2600.py auto-detects and uses this
# when available (byte-identical output, ~55x faster than the Python oracle).
csim:
	$(MAKE) -C $(CSIM_DIR)

csim-clean:
	$(MAKE) -C $(CSIM_DIR) clean

clean:
	rm -f $(BINS)
