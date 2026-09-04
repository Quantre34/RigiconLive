# Rigicon Live - macOS / Linux build
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
SRC      := src/main.c src/crypto.c src/net.c src/term.c src/notify.c
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

.PHONY: all clean run

all: $(OUT)

$(OUT): $(SRC) $(HDR)
	@mkdir -p $(dir $(OUT))
	$(CC) $(CFLAGS) -o $(OUT) $(SRC) $(LDFLAGS)
	@echo "Built: $(OUT)"

clean:
	rm -f dist/macos/RigiconLive dist/linux/RigiconLive

run: $(OUT)
	$(OUT)
