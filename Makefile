CFLAGS=-Wall -Wextra -Werror -O2 $(shell pkg-config --cflags libnotify)
LDFLAGS=$(shell pkg-config --libs libnotify)

SOURCES=$(wildcard *.c)
EXECUTABLES=$(patsubst %.c,%,$(SOURCES))

all: $(EXECUTABLES)

clean:
	-rm -f $(EXECUTABLES)
