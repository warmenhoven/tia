CC     ?= cc
CFLAGS ?= -O2 -Wall -Wextra -I../../src

SRC_DIR      := ../../src
CORE_SOURCES := $(SRC_DIR)/cpu.c $(SRC_DIR)/tia.c \
                $(SRC_DIR)/cart.c $(SRC_DIR)/riot.c

BINS := run_tia_c

.PHONY: all clean
all: $(BINS)

$(BINS): %: %.c $(CORE_SOURCES)
	$(CC) $(CFLAGS) -o $@ $< $(CORE_SOURCES)

clean:
	rm -f $(BINS)
