CC      ?= gcc
CFLAGS  ?= -O3 -Wall -Wextra -march=native -pthread
LDFLAGS ?= -pthread -lm
PREFIX  ?= /usr/local

.PHONY: all static test clean install uninstall

all: dramtop

dramtop: dramtop.c
	$(CC) $(CFLAGS) dramtop.c -o dramtop $(LDFLAGS)
	strip dramtop || true

static: dramtop.c
	$(CC) $(CFLAGS) -static dramtop.c -o dramtop-static $(LDFLAGS)
	strip dramtop-static || true

test: dramtop
	./dramtop --version
	./dramtop --help >/dev/null

install: dramtop
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 dramtop $(DESTDIR)$(PREFIX)/bin/dramtop

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/dramtop

clean:
	rm -f dramtop dramtop-static
