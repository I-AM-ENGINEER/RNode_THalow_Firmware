# tests/Makefile — Host-side unit tests for pure-C modules
#
# These tests compile firmware modules (or inlined copies) against host
# stubs to verify protocol logic without needing an ESP32. Run with:
#   make && make test

CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -O2 -g
TESTS = test_rns_framing test_kiss test_dns_overflow

.PHONY: all test clean

all: $(TESTS)

%: %.c
	$(CC) $(CFLAGS) -o $@ $<

test: $(TESTS)
	@total_pass=0; total_fail=0; \
	for t in $(TESTS); do \
		echo ""; echo "--- Running $$t ---"; \
		./$$t; rc=$$?; \
		if [ $$rc -ne 0 ]; then total_fail=$$((total_fail+1)); \
		else total_pass=$$((total_pass+1)); fi; \
	done; \
	echo ""; echo "========================="; \
	echo "Test suites: $$((total_pass+total_fail)) total, $$total_pass passed, $$total_fail failed"; \
	echo "========================="; \
	if [ $$total_fail -gt 0 ]; then exit 1; fi

clean:
	rm -f $(TESTS)
