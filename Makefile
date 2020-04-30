CFLAGS+=-pedantic -Wall -Wextra -Werror $(shell pkg-config --cflags libnotify libsystemd)
LDFLAGS=$(shell pkg-config --libs libnotify libsystemd)

SOURCES=$(wildcard *.c)
EXECUTABLES=$(patsubst %.c,%,$(SOURCES))

all: $(EXECUTABLES)

# Noisy clang build that's expected to fail, but can be useful to find corner
# cases.
clang-everything: CC=clang
clang-everything: CFLAGS+=-Weverything
clang-everything: all

sanitisers: CC=gcc
sanitisers: CFLAGS+=-fsanitize=address -fsanitize=undefined -fno-omit-frame-pointer -ggdb
sanitisers: all

debug: CFLAGS+=-Og -ggdb -fno-omit-frame-pointer
debug: all

clang-tidy:
	-clang-tidy psi-notify.c -checks=-clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling -- $(CFLAGS) $(LDFLAGS)

clean:
	-rm -f $(EXECUTABLES)
