.POSIX:

CC = cc
PKG_CONFIG = pkg-config

PKGS = wayland-server xkbcommon libinput pixman-1 fcft wlroots
CFLAGS = -g -O2 `$(PKG_CONFIG) --cflags $(PKGS)`
LDFLAGS = `$(PKG_CONFIG) --libs $(PKGS)`

all: menu

menu: main.c
	$(CC) $(CFLAGS) main.c -o menu $(LDFLAGS)

clean:
	rm -f menu
