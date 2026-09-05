.POSIX:

CC      = cc

VERSION != git describe --tags --always --dirty 2>/dev/null || echo dev

CFLAGS  = -std=c11 -pedantic -Wall -Wextra -Os -D_POSIX_C_SOURCE=200809L \
          -DHWEB_VERSION='"$(VERSION)"' -Wno-deprecated-declarations
WKFLAGS != pkg-config --cflags webkit2gtk-4.1 | sed 's/-I/-isystem /g'
WKLIBS  != pkg-config --libs webkit2gtk-4.1
EXFLAGS != pkg-config --cflags webkit2gtk-web-extension-4.1 | sed 's/-I/-isystem /g'
EXLIBS  != pkg-config --libs webkit2gtk-web-extension-4.1
BINDIR  = $(HOME)/.local/bin
APPDIR  = $(HOME)/.local/share/applications

all: hweb hweb-ext.so hwebc

hweb: hweb.c history.c history.h config.h args.h
	$(CC) $(CFLAGS) $(WKFLAGS) -o $@ hweb.c history.c $(WKLIBS)

# the control client: plain C, no GTK
hwebc: hwebc.c
	$(CC) $(CFLAGS) -o $@ hwebc.c

# loaded into WebKit's web process; hweb points WebKit at its own
# directory (resolved through the install symlink) to find it
hweb-ext.so: hweb-ext.c
	$(CC) $(CFLAGS) $(EXFLAGS) -shared -fPIC -o $@ hweb-ext.c $(EXLIBS)

install: all
	mkdir -p $(BINDIR) $(APPDIR)
	ln -sf "$$(pwd)/hweb" $(BINDIR)/hweb
	ln -sf "$$(pwd)/hwebc" $(BINDIR)/hwebc
	sed "s|^Exec=hweb|Exec=$(BINDIR)/hweb|" hweb.desktop > $(APPDIR)/hweb.desktop
	update-desktop-database $(APPDIR) 2>/dev/null || true
	xdg-settings set default-web-browser hweb.desktop

uninstall:
	rm -f $(BINDIR)/hweb $(BINDIR)/hwebc $(APPDIR)/hweb.desktop

clean:
	rm -f hweb hweb-ext.so hwebc

.PHONY: all install uninstall clean
