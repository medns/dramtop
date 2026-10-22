CC      ?= cc
STRIP   ?= strip
CFLAGS  ?= -O2 -Wall -Wextra
PREFIX  ?= /usr/local
VERSION ?=

override CFLAGS += -std=gnu11 -pthread
ifneq ($(VERSION),)
override CPPFLAGS += -DDRAMTOP_VERSION='"$(VERSION)"'
endif

# The compiler's target, not the build host, picks the platform code, so cross compilers work too.
TARGET   := $(shell $(CC) -dumpmachine 2>/dev/null)
PLATFORM := $(if $(findstring linux,$(TARGET)),linux,$(if $(findstring darwin,$(TARGET)),macos))
ifeq ($(PLATFORM),)
ifneq ($(MAKECMDGOALS),clean)
$(error unsupported build target '$(TARGET)': dramtop builds on Linux and macOS)
endif
endif

EXTRA_macos  := src/macos_metal.m
CFLAGS_macos := -fobjc-arc
LDLIBS_macos := -framework CoreFoundation -framework IOKit -framework Foundation -framework Metal
override CFLAGS += $(CFLAGS_$(PLATFORM))
override LDLIBS += $(LDLIBS_$(PLATFORM))

HDRS     := src/dramtop.h
PLAT_SRC := src/os_posix.c src/$(PLATFORM).c $(EXTRA_$(PLATFORM))
SRCS     := src/dramtop.c $(PLAT_SRC)

TESTS       := tests/test_diagnose tests/test_output tests/test_cli
TESTS_linux := tests/test_counter
TESTS       += $(TESTS_$(PLATFORM))

.PHONY: all static test accuracy install uninstall clean

all: dramtop

dramtop: $(SRCS) $(HDRS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $(SRCS) -o $@ $(LDLIBS)

static: $(SRCS) $(HDRS)
	$(if $(filter linux,$(PLATFORM)),,$(error static binaries can only be built for Linux))
	$(CC) $(CPPFLAGS) $(CFLAGS) -static $(LDFLAGS) $(SRCS) -o dramtop-static $(LDLIBS)
	$(STRIP) dramtop-static

test: dramtop $(TESTS)
	./dramtop --version
	./dramtop --help > /dev/null
	@for t in $(TESTS); do ./$$t || exit 1; done

# Each test includes the source file it tests, so it can reach static functions.
tests/test_counter: tests/test_counter.c tests/check.h src/linux.c $(HDRS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $< -o $@ $(LDLIBS)

tests/test_%: tests/test_%.c tests/check.h $(SRCS) $(HDRS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $< $(PLAT_SRC) -o $@ $(LDLIBS)

accuracy: dramtop tests/accuracy

tests/accuracy: tests/accuracy.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $< -o $@ $(LDLIBS)

install: dramtop
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 dramtop $(DESTDIR)$(PREFIX)/bin/dramtop

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/dramtop

clean:
	rm -f dramtop dramtop-static tests/accuracy $(basename $(wildcard tests/test_*.c))
