# Standalone build (no CMake, no maxivmac): `make -C tools/ethhub`
# Sanitizer build for tests: `make SAN=1`
CC ?= cc
CFLAGS ?= -O2
CFLAGS += -std=c11 -Wall -Wextra -Werror
ifdef SAN
CFLAGS += -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined
endif
ethhub: ethhub.c
	$(CC) $(CFLAGS) -o $@ ethhub.c
clean:
	rm -f ethhub
.PHONY: clean
