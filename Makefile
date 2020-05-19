CFLAGS+=-std=gnu11 -O2 -pedantic -Wall -Wextra -Werror $(shell pkg-config --cflags libnotify)
LDFLAGS=$(shell pkg-config --libs libnotify)

WANT_SD_NOTIFY=1
HAS_LIBSYSTEMD=$(shell pkg-config libsystemd && echo 1 || echo 0)

ifeq ($(HAS_LIBSYSTEMD),0)
$(warning libsystemd not found, setting WANT_SD_NOTIFY=0)
WANT_SD_NOTIFY=0
endif

ifeq ($(WANT_SD_NOTIFY),1)
CFLAGS+=-DWANT_SD_NOTIFY $(shell pkg-config --cflags libsystemd)
LDFLAGS+=$(shell pkg-config --libs libsystemd)
endif

SOURCES=$(wildcard *.c)
EXECUTABLES=$(patsubst %.c,%,$(SOURCES))

.PHONY: test

all: $(EXECUTABLES)

%: %.c
	$(CC) $(CFLAGS) $< -o $@ $(LIBS) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) $< -c -o $@

%: %.o
	$(CC) $< -o $@ $(LIBS) $(LDFLAGS)

# Noisy clang build that's expected to fail, but can be useful to find corner
# cases.
clang-everything: CC=clang
clang-everything: CFLAGS+=-Weverything -Wno-disabled-macro-expansion -Wno-padded -Wno-unused-macros
clang-everything: all

sanitisers: CFLAGS+=-fsanitize=address -fsanitize=undefined
sanitisers: debug

debug: CFLAGS+=-Og -ggdb -fno-omit-frame-pointer
debug: all

afl: CC=afl-gcc
afl: CFLAGS+=-DWANT_FUZZER
afl: export AFL_HARDEN=1 AFL_USE_ASAN=1
afl: sanitisers

fuzz-configs: afl
	fuzz/configs/run

fuzz-pressures: afl
	fuzz/pressures/run

clang-tidy:
	# DeprecatedOrUnsafeBufferHandling: See https://stackoverflow.com/a/50724865/945780
	clang-tidy psi-notify.c -checks=-clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling -- $(CFLAGS) $(LDFLAGS)

test:
	$(CC) $(CFLAGS) test/test.c -o test/test $(LIBS) $(LDFLAGS)
	test/test

clean:
	rm -f $(EXECUTABLES) test/test
