CC ?= cc
PKG_CONFIG ?= pkg-config

PKGS = scenefx-0.5 wlroots-0.20 wayland-server xkbcommon pixman-1

# Pinned baseline: plain x86-64 (v1) so binaries run on any 64-bit CPU.
# Never use -march=native here; distro CFLAGS/env do not apply to direct make builds,
# and this guard keeps it that way even if someone exports CFLAGS.
CFLAGS := -g -O3 -Werror -march=x86-64 -mtune=generic \
          $(shell $(PKG_CONFIG) --cflags $(PKGS)) \
          -I. -DWLR_USE_UNSTABLE
LDLIBS := $(shell $(PKG_CONFIG) --libs $(PKGS)) -lm

all: amberwm ambermsg

wlr-layer-shell-unstable-v1-protocol.h: protocols/wlr-layer-shell-unstable-v1.xml
	wayland-scanner server-header $< $@

wlr-output-power-management-unstable-v1-protocol.h: protocols/wlr-output-power-management-unstable-v1.xml
	wayland-scanner server-header $< $@

amberwm.o: amberwm.c wlr-layer-shell-unstable-v1-protocol.h wlr-output-power-management-unstable-v1-protocol.h
	$(CC) -c $< $(CFLAGS) -o $@

amberwm: amberwm.o
	$(CC) $^ $(LDFLAGS) $(LDLIBS) -o $@

ambermsg: ambermsg.c
	$(CC) -g -O2 -Werror -march=x86-64 -mtune=generic $< -o $@

clean:
	rm -f amberwm ambermsg amberwm.o wlr-layer-shell-unstable-v1-protocol.h wlr-output-power-management-unstable-v1-protocol.h

.PHONY: all clean
