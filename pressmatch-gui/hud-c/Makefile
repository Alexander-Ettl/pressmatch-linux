CC      ?= gcc
SCANNER ?= wayland-scanner
PKG     ?= pkg-config

WL_PROTO_DIR := $(shell $(PKG) --variable=pkgdatadir wayland-protocols)

PKGS := wayland-client pangocairo cairo libsystemd
CFLAGS  := -std=c11 -D_GNU_SOURCE -O2 -Wall -Wextra -Wno-unused-parameter -Igen $(shell $(PKG) --cflags $(PKGS))
LDFLAGS := $(shell $(PKG) --libs $(PKGS)) -lm

PROTOS := \
	xdg-shell \
	wlr-layer-shell-unstable-v1 \
	viewporter \
	fractional-scale-v1 \
	single-pixel-buffer-v1

GEN_HDRS := $(patsubst %,gen/%-client-protocol.h,$(PROTOS))
GEN_SRCS := $(patsubst %,gen/%-protocol.c,$(PROTOS))
GEN_OBJS := $(patsubst %,gen/%-protocol.o,$(PROTOS))

SRCS := hud_main.c hud_render.c
OBJS := $(SRCS:.c=.o)

TARGET := pressmatch-hud

.PHONY: all clean

all: $(TARGET)

gen:
	mkdir -p gen

gen/xdg-shell-client-protocol.h: $(WL_PROTO_DIR)/stable/xdg-shell/xdg-shell.xml | gen
	$(SCANNER) client-header $< $@
gen/xdg-shell-protocol.c: $(WL_PROTO_DIR)/stable/xdg-shell/xdg-shell.xml | gen
	$(SCANNER) private-code $< $@

gen/wlr-layer-shell-unstable-v1-client-protocol.h: protocols/wlr-layer-shell-unstable-v1.xml | gen
	$(SCANNER) client-header $< $@
gen/wlr-layer-shell-unstable-v1-protocol.c: protocols/wlr-layer-shell-unstable-v1.xml | gen
	$(SCANNER) private-code $< $@

gen/viewporter-client-protocol.h: $(WL_PROTO_DIR)/stable/viewporter/viewporter.xml | gen
	$(SCANNER) client-header $< $@
gen/viewporter-protocol.c: $(WL_PROTO_DIR)/stable/viewporter/viewporter.xml | gen
	$(SCANNER) private-code $< $@

gen/fractional-scale-v1-client-protocol.h: $(WL_PROTO_DIR)/staging/fractional-scale/fractional-scale-v1.xml | gen
	$(SCANNER) client-header $< $@
gen/fractional-scale-v1-protocol.c: $(WL_PROTO_DIR)/staging/fractional-scale/fractional-scale-v1.xml | gen
	$(SCANNER) private-code $< $@

gen/single-pixel-buffer-v1-client-protocol.h: $(WL_PROTO_DIR)/staging/single-pixel-buffer/single-pixel-buffer-v1.xml | gen
	$(SCANNER) client-header $< $@
gen/single-pixel-buffer-v1-protocol.c: $(WL_PROTO_DIR)/staging/single-pixel-buffer/single-pixel-buffer-v1.xml | gen
	$(SCANNER) private-code $< $@

$(OBJS): $(GEN_HDRS) hud_render.h

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

gen/%-protocol.o: gen/%-protocol.c gen/%-client-protocol.h
	$(CC) $(CFLAGS) -c -o $@ $<

$(TARGET): $(OBJS) $(GEN_OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

clean:
	rm -rf gen $(OBJS) $(TARGET)
