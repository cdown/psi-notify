CFLAGS=-pedantic -Wall -Wextra -Werror $(shell pkg-config --cflags libnotify)
LDFLAGS=$(shell pkg-config --libs libnotify)

# Noisy clang build that's expected to fail, but can be useful to find corner cases.
clang : CC=clang
clang : CFLAGS=-pedantic -Weverything -Werror $(shell pkg-config --cflags libnotify)

SOURCES=$(wildcard *.c)
EXECUTABLES=$(patsubst %.c,%,$(SOURCES))

all: $(EXECUTABLES)

clang: all

clang-tidy: 
	-clang-tidy psi-notify.c -checks=-clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling -- $(CFLAGS) $(LDFLAGS)

clean:
	-rm -f $(EXECUTABLES)
