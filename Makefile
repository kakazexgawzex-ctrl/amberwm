CC ?= cc
PKG_CONFIG ?= pkg-config

PKGS = wlroots-0.20 wayland-server xkbcommon

# Pinned baseline: plain x86-64 (v1) so binaries run on any 64-bit CPU.
# Never use -march=native here; distro CFLAGS/env do not apply to direct make builds,
# and this guard keeps it that way even if someone exports CFLAGS.
CFLAGS := -g -O2 -Werror -march=x86-64 -mtune=generic \
          $(shell $(PKG_CONFIG) --cflags $(PKGS)) \
          -I. -DWLR_USE_UNSTABLE
LDLIBS := $(shell $(PKG_CONFIG) --libs $(PKGS))

all: amberwm

wlr-layer-shell-unstable-v1-protocol.h: protocols/wlr-layer-shell-unstable-v1.xml
	wayland-scanner server-header $< $@

amberwm.o: amberwm.c wlr-layer-shell-unstable-v1-protocol.h
	$(CC) -c $< $(CFLAGS) -o $@

amberwm: amberwm.o
	$(CC) $^ $(LDFLAGS) $(LDLIBS) -o $@

clean:
	rm -f amberwm amberwm.o wlr-layer-shell-unstable-v1-protocol.h

.PHONY: all clean
