CC = gcc
CXX = g++
CXXFLAGS = -Wall -Wextra -Wno-unused-parameter
LIBS = -lwayland-client -lcairo -lpangocairo-1.0 -lpango-1.0 -lgobject-2.0 -lglib-2.0

INCLUDES = $(shell pkg-config --cflags wayland-client cairo pango pangocairo)

WLR_PROTOCOL = wlr-layer-shell-unstable-v1.xml
XDG_SHELL_PROTOCOL = xdg-shell.xml

all: rmenu

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
	$(CC) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# Compile wlr-layer-shell protocol
wlr-layer-shell-unstable-v1-client-protocol.o: wlr-layer-shell-unstable-v1-client-protocol.c
	$(CC) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# Compile main
main.o: main.cc wlr-layer-shell-unstable-v1-client-protocol.h xdg-shell-client-protocol.h config.h
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c main.cc -o $@
	#
# Compile other
wayland.o: wayland.cc wlr-layer-shell-unstable-v1-client-protocol.h xdg-shell-client-protocol.h config.h
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c wayland.cc -o $@

# Link
rmenu: main.o wayland.o wlr-layer-shell-unstable-v1-client-protocol.o xdg-shell-client-protocol.o
	$(CXX) $(CXXFLAGS) $^ $(LIBS) -o $@

clean:
	rm -f rmenu *.o wlr-layer-shell-unstable-v1-client-protocol.h wlr-layer-shell-unstable-v1-client-protocol.c xdg-shell-client-protocol.h xdg-shell-client-protocol.c

.PHONY: all clean

install: rmenu
	install -Dm755 rmenu /usr/local/bin/rmenu
