CFLAGS+=-pedantic -Wall -Wextra -Werror $(shell pkg-config --cflags libnotify)
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

sanitisers: CC=gcc
sanitisers: CFLAGS+=-fsanitize=address -fsanitize=undefined
sanitisers: debug

debug: CFLAGS+=-Og -ggdb -fno-omit-frame-pointer -fsanitize=address -fsanitize=undefined
debug: all

afl: CC=afl-gcc
afl: CFLAGS+=-DWANT_FUZZER
afl: export AFL_HARDEN=1 AFL_USE_ASAN=1
afl: debug

fuzz-configs: afl
	mkdir -p fuzz/configs/generated
	UBSAN_OPTIONS=halt_on_error=1:abort_on_error=1 XDG_CONFIG_DIR=fuzz/configs/generated FUZZ=1 afl-fuzz -i fuzz/configs/testcases -o fuzz/configs/results -f fuzz/configs/generated/psi-notify -m none ./psi-notify

clang-tidy:
	clang-tidy psi-notify.c -checks=-clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling -- $(CFLAGS) $(LDFLAGS)

clean:
	rm -f $(EXECUTABLES)
