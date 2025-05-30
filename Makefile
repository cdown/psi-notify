CFLAGS:=-std=gnu11 -O2 -pedantic -Wall -Wextra -Wwrite-strings -Warray-bounds -Wconversion -Wstrict-prototypes -Werror $(shell pkg-config --cflags libnotify) $(CFLAGS)
CPPFLAGS:=$(CPPFLAGS)
LDFLAGS:=$(shell pkg-config --libs libnotify) $(LDFLAGS)

INSTALL:=install
prefix:=/usr/local
bindir:=$(prefix)/bin
datarootdir:=$(prefix)/share
mandir:=$(datarootdir)/man


SOURCES=$(wildcard *.c)
EXECUTABLES=$(patsubst %.c,%,$(SOURCES))

.PHONY: test

all: $(EXECUTABLES)

%: %.c %.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(filter %.c,$<) -o $@ $(LIBS) $(LDFLAGS)

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -c -o $@

%: %.o
	$(CC) $< -o $@ $(LIBS) $(LDFLAGS)

# Noisy clang build that's expected to fail, but can be useful to find corner
# cases.
clang-everything: CC=clang
clang-everything: CFLAGS+=-Weverything -Wno-disabled-macro-expansion -Wno-padded -Wno-unused-macros -Wno-covered-switch-default -Wno-unsafe-buffer-usage
clang-everything: all

sanitisers: CFLAGS+=-fsanitize=address -fsanitize=undefined -fanalyzer
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

install: all
	mkdir -p $(DESTDIR)$(bindir)/
	$(INSTALL) -pt $(DESTDIR)$(bindir)/ $(EXECUTABLES)
	$(INSTALL) -Dp -m 644 psi-notify.service $(DESTDIR)$(prefix)/lib/systemd/user/psi-notify.service
	$(INSTALL) -Dp -m 644 psi-notify.1 $(DESTDIR)$(mandir)/man1/psi-notify.1

test: CFLAGS+=-D_FORTIFY_SOURCE=2 -fsanitize=address -fsanitize=undefined -Og -ggdb -fno-omit-frame-pointer
test:
	$(CC) $(CPPFLAGS) $(CFLAGS) test/test.c -o test/test $(LIBS) $(LDFLAGS)
	test/test

clean:
	rm -f $(EXECUTABLES) test/test
