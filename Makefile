# Rigicon Live - macOS / Linux build
# Disable Make's implicit rules (e.g. %: %.sh) so `make install` does not
# silently create a bogus ./install file from install.sh.
MAKEFLAGS += --no-builtin-rules
.SUFFIXES:

CC       ?= cc
CFLAGS   ?= -O2 -Wall -Wextra -Wno-unused-parameter -std=c99

UNAME_S_TMP := $(shell uname -s)
ifeq ($(UNAME_S_TMP),Darwin)
    CFLAGS  += -D_DARWIN_C_SOURCE
endif
ifeq ($(UNAME_S_TMP),Linux)
    CFLAGS  += -D_GNU_SOURCE
endif
LDFLAGS  ?=
SRC      := src/main.c src/crypto.c src/net.c src/term.c src/notify.c src/file.c
HDR      := $(wildcard src/*.h)

UNAME_S  := $(shell uname -s)

ifeq ($(UNAME_S),Linux)
    LDFLAGS += -lpthread
    OUT     := dist/linux/RigiconLive
endif
ifeq ($(UNAME_S),Darwin)
    LDFLAGS += -lpthread
    OUT     := dist/macos/RigiconLive
endif

ifndef OUT
    $(error Unsupported platform: $(UNAME_S). Use build.bat on Windows.)
endif

PREFIX ?= /usr/local
BINDIR := $(PREFIX)/bin

.PHONY: all clean run install uninstall

all: $(OUT)

$(OUT): $(SRC) $(HDR)
	@mkdir -p $(dir $(OUT))
	$(CC) $(CFLAGS) -o $(OUT) $(SRC) $(LDFLAGS)
	@echo "Built: $(OUT)"

clean:
	rm -f dist/macos/RigiconLive dist/linux/RigiconLive

run: $(OUT)
	$(OUT)

# `sudo make install` copies the built binary to /usr/local/bin/RigiconLive
# so it lives on your PATH. Override with:  sudo make install PREFIX=/opt/rigicon
install: $(OUT)
	@mkdir -p $(BINDIR)
	install -m 755 $(OUT) $(BINDIR)/RigiconLive
	@echo "Installed: $(BINDIR)/RigiconLive"
	@echo "Type 'RigiconLive' from a new terminal to run."

uninstall:
	rm -f $(BINDIR)/RigiconLive
	@echo "Removed: $(BINDIR)/RigiconLive"
