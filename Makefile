CC      ?= cc
STRIP   ?= strip
CFLAGS  ?= -O2 -Wall -Wextra
PREFIX  ?= /usr/local
VERSION ?=

override CFLAGS += -std=gnu11 -pthread
ifneq ($(VERSION),)
override CPPFLAGS += -DDRAMTOP_VERSION='"$(VERSION)"'
endif

.PHONY: all static test install uninstall clean

all: dramtop

dramtop: dramtop.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $< -o $@ $(LDLIBS)

static: dramtop.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -static $(LDFLAGS) $< -o dramtop-static $(LDLIBS)
	$(STRIP) dramtop-static

test: dramtop
	./dramtop --version
	./dramtop --help > /dev/null

install: dramtop
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 dramtop $(DESTDIR)$(PREFIX)/bin/dramtop

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/dramtop

clean:
	rm -f dramtop dramtop-static
