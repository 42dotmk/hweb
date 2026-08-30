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

all: hweb hweb-ext.so

hweb: hweb.c history.c history.h config.h
	$(CC) $(CFLAGS) $(WKFLAGS) -o $@ hweb.c history.c $(WKLIBS)

# loaded into WebKit's web process; hweb points WebKit at its own
# directory (resolved through the install symlink) to find it
hweb-ext.so: hweb-ext.c
	$(CC) $(CFLAGS) $(EXFLAGS) -shared -fPIC -o $@ hweb-ext.c $(EXLIBS)

install: all
	mkdir -p $(BINDIR)
	ln -sf "$$(pwd)/hweb" $(BINDIR)/hweb

uninstall:
	rm -f $(BINDIR)/hweb

clean:
	rm -f hweb hweb-ext.so

.PHONY: all install uninstall clean
