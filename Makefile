CFLAGS:=-std=gnu11 -O2 -pedantic -Wall -Wextra -Werror $(shell pkg-config --cflags libnotify) $(CFLAGS)
CPPFLAGS:=$(CPPFLAGS)
LDFLAGS:=$(shell pkg-config --libs libnotify) $(LDFLAGS)

INSTALL:=install
prefix:=/usr/local
bindir:=$(prefix)/bin
datarootdir:=$(prefix)/share
mandir:=$(datarootdir)/man


WANT_SD_NOTIFY=1
HAS_LIBSYSTEMD=$(shell pkg-config libsystemd && echo 1 || echo 0)

ifeq ($(HAS_LIBSYSTEMD),0)
$(warning libsystemd not found, setting WANT_SD_NOTIFY=0)
WANT_SD_NOTIFY=0
endif

HAS_SYSTEMD=$(shell pkg-config systemd && echo 1 || echo 0)

ifeq ($(HAS_SYSTEMD),1)
SYSTEMD_USER_UNIT_DIR:=$(shell pkg-config --define-variable=prefix=$(prefix) --variable systemd_user_unit_dir systemd)
else
$(warning systemd not found, using default for SYSTEMD_USER_UNIT_DIR)
SYSTEMD_USER_UNIT_DIR:=$(prefix)/lib/systemd/user
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
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@ $(LIBS) $(LDFLAGS)

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -c -o $@

%: %.o
	$(CC) $< -o $@ $(LIBS) $(LDFLAGS)

# Noisy clang build that's expected to fail, but can be useful to find corner
# cases.
clang-everything: CC=clang
clang-everything: CFLAGS+=-Weverything -Wno-disabled-macro-expansion -Wno-padded -Wno-unused-macros -Wno-covered-switch-default
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

install: all
	mkdir -p $(DESTDIR)$(bindir)/
	$(INSTALL) -pt $(DESTDIR)$(bindir)/ $(EXECUTABLES)
	$(INSTALL) -Dp -m 644 psi-notify.service $(DESTDIR)$(SYSTEMD_USER_UNIT_DIR)/psi-notify.service
	$(INSTALL) -Dp -m 644 psi-notify.1 $(DESTDIR)$(mandir)/man1/psi-notify.1

test: CFLAGS+=-D_FORTIFY_SOURCE=2 -fsanitize=address -fsanitize=undefined -Og -ggdb -fno-omit-frame-pointer
test:
	$(CC) $(CPPFLAGS) $(CFLAGS) test/test.c -o test/test $(LIBS) $(LDFLAGS)
	test/test

clean:
	rm -f $(EXECUTABLES) test/test
