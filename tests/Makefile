# tests/Makefile — Host-side unit tests (Level 1)
#
# Pure-C firmware modules compiled against host stubs. Run: make test

# Plain gcc is sufficient: all host tests are self-contained (gzip vectors
# are pre-generated into gzip_vectors.h, no zlib dependency).
CC      = gcc
CFLAGS  = -Wall -Wextra -std=c11 -O2 -g
TESTS   = test_rns_framing test_kiss test_dns_overflow \
          test_gzip_inflate test_switch_ring

.PHONY: all test clean

all: $(TESTS)

test_gzip_inflate: test_gzip_inflate.c ../src/gzip_inflate.c
	$(CC) $(CFLAGS) -I../inc -o $@ $^

test_switch_ring: test_switch_ring.c ../src/switch.c \
                  stub/freertos/FreeRTOS.h freertos_host_mock.h
	$(CC) $(CFLAGS) -I. -Istub -I../inc -o $@ test_switch_ring.c

%: %.c
	$(CC) $(CFLAGS) -o $@ $<

test: $(TESTS)
	@total_fail=0; \
	for t in $(TESTS); do \
		echo ""; echo "--- $$t ---"; \
		./$$t || total_fail=$$((total_fail+1)); \
	done; \
	echo ""; echo "=========================="; \
	if [ $$total_fail -gt 0 ]; then \
		echo "FAILED suites: $$total_fail"; exit 1; \
	else echo "ALL SUITES PASSED"; fi

clean:
	rm -f $(TESTS)
