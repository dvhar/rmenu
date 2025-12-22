CC = gcc
CFLAGS = -Wall -Wextra -Wno-unused-parameter
LIBS = -lwayland-client -lcairo -lpangocairo-1.0 -lpango-1.0 -lgobject-2.0 -lglib-2.0

# Get include paths from pkg-config
INCLUDES = $(shell pkg-config --cflags wayland-client cairo pango pangocairo)

WLR_PROTOCOL = /usr/share/wlr-protocols/unstable/wlr-layer-shell-unstable-v1.xml
XDG_SHELL_PROTOCOL = /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml

all: hello-world

# Generate xdg-shell protocol files
xdg-shell-client-protocol.h:
	wayland-scanner client-header $(XDG_SHELL_PROTOCOL) $@

xdg-shell-client-protocol.c: xdg-shell-client-protocol.h
	wayland-scanner private-code $(XDG_SHELL_PROTOCOL) $@

# Generate wlr-layer-shell protocol files
wlr-layer-shell-unstable-v1-client-protocol.h: xdg-shell-client-protocol.h
	wayland-scanner client-header $(WLR_PROTOCOL) $@

wlr-layer-shell-unstable-v1-client-protocol.c: wlr-layer-shell-unstable-v1-client-protocol.h
	wayland-scanner private-code $(WLR_PROTOCOL) $@

# Compile xdg-shell protocol
xdg-shell-client-protocol.o: xdg-shell-client-protocol.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Compile wlr-layer-shell protocol
wlr-layer-shell-unstable-v1-client-protocol.o: wlr-layer-shell-unstable-v1-client-protocol.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Compile main
main.o: main.c wlr-layer-shell-unstable-v1-client-protocol.h xdg-shell-client-protocol.h
	$(CC) $(CFLAGS) $(INCLUDES) -c main.c -o $@

# Link
hello-world: main.o wlr-layer-shell-unstable-v1-client-protocol.o xdg-shell-client-protocol.o
	$(CC) $(CFLAGS) $^ $(LIBS) -o $@

clean:
	rm -f hello-world *.o wlr-layer-shell-unstable-v1-client-protocol.h wlr-layer-shell-unstable-v1-client-protocol.c xdg-shell-client-protocol.h xdg-shell-client-protocol.c

.PHONY: all clean
