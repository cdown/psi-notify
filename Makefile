CFLAGS=-pedantic -Wall -Wextra -Werror $(shell pkg-config --cflags libnotify)
LDFLAGS=$(shell pkg-config --libs libnotify)

# noisy clang build that's expected to fail, but can be useful to find corner cases.
#CC=clang
#CFLAGS=-pedantic -Weverything $(shell pkg-config --cflags libnotify)

SOURCES=$(wildcard *.c)
EXECUTABLES=$(patsubst %.c,%,$(SOURCES))

all: $(EXECUTABLES)

clean:
	-rm -f $(EXECUTABLES)
