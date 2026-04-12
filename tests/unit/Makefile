CC     ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra -I../../src

SRC_DIR      := ../../src
CORE_SOURCES := $(SRC_DIR)/cpu.c $(SRC_DIR)/riot.c

TESTS := $(wildcard test_*.c)
BINS  := $(TESTS:.c=)

.PHONY: all test clean
all: test

test: $(BINS)
	@if [ -z "$(BINS)" ]; then echo "no unit tests yet"; exit 0; fi; \
	fails=0; \
	for t in $(BINS); do \
		echo "== $$t =="; \
		./$$t || fails=$$((fails+1)); \
	done; \
	if [ $$fails -ne 0 ]; then echo "$$fails failed"; exit 1; fi

%: %.c $(CORE_SOURCES)
	$(CC) $(CFLAGS) -o $@ $< $(CORE_SOURCES)

clean:
	rm -f $(BINS)
