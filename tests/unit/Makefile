CC     ?= cc
CFLAGS ?= -O2 -Wall -Wextra -I../../src

SRC_DIR      := ../../src
CORE_SOURCES := $(SRC_DIR)/cpu.c $(SRC_DIR)/riot.c $(SRC_DIR)/tia.c \
                $(SRC_DIR)/cart.c $(SRC_DIR)/bus.c

TESTS := $(wildcard test_*.c)
BINS  := $(TESTS:.c=)

.PHONY: all test clean
all: test

test: $(BINS)
	@if [ -z "$(BINS)" ]; then echo "no unit tests yet"; exit 0; fi; \
	fails=0; skips=0; \
	for t in $(BINS); do \
		echo "== $$t =="; \
		./$$t; rc=$$?; \
		if [ $$rc -eq 77 ]; then skips=$$((skips+1)); \
		elif [ $$rc -ne 0 ]; then fails=$$((fails+1)); fi; \
	done; \
	if [ $$skips -ne 0 ]; then echo "$$skips skipped"; fi; \
	if [ $$fails -ne 0 ]; then echo "$$fails failed"; exit 1; fi

$(BINS): %: %.c $(CORE_SOURCES)
	$(CC) $(CFLAGS) -o $@ $< $(CORE_SOURCES)

clean:
	rm -f $(BINS)
