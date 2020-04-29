CFLAGS=-pedantic -Wall -Wextra -Wduplicated-cond -Wduplicated-branches -Wlogical-op -Wrestrict -Wnull-dereference -Wjump-misses-init -Wdouble-promotion -Wshadow -Wformat=2 -Werror -O2 $(shell pkg-config --cflags libnotify)
LDFLAGS=$(shell pkg-config --libs libnotify)

# noisy clang build that's expected to fail, but can be useful to find corner cases.
#CC=clang
#CFLAGS=-g -pedantic -Weverything $(shell pkg-config --cflags libnotify)

SOURCES=$(wildcard *.c)
EXECUTABLES=$(patsubst %.c,%,$(SOURCES))

all: $(EXECUTABLES)

clean:
	-rm -f $(EXECUTABLES)
