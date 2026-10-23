# Optic DE. On a Debian PC: make && sudo make install (see docs/SETUP-DEBIAN.md).
# On Android, Optic Suite's runtime builds it inside its Debian (Emulator -> Linux -> DESKTOP).
PKGS   = x11 xft xext xcursor libpng fontconfig
CFLAGS = -O2 -Wall -Wno-misleading-indentation $(shell pkg-config --cflags $(PKGS))
LIBS   = $(shell pkg-config --libs $(PKGS)) -lm

all: optic-de optic-settings optic-term optic-files libsysvshm.so

optic-de: de.c ui.c ui.h
	$(CC) $(CFLAGS) -o $@ de.c ui.c $(LIBS)

optic-settings: settings.c ui.c ui.h
	$(CC) $(CFLAGS) -o $@ settings.c ui.c $(LIBS)

optic-term: term.c ui.c ui.h
	$(CC) $(CFLAGS) -o $@ term.c ui.c $(LIBS)

optic-files: files.c ui.c ui.h
	$(CC) $(CFLAGS) -o $@ files.c ui.c $(LIBS)

libsysvshm.so: sysvshm.c
	$(CC) -O2 -Wall -shared -fPIC -o $@ sysvshm.c

install: all
	install -d /opt/optic-de/bin /opt/optic-de/share /opt/optic-de/lib
	install -m 755 libsysvshm.so /opt/optic-de/lib/
	install -m 755 optic-de optic-settings optic-term optic-files optic-session optic-firefox /opt/optic-de/bin/
	install -m 644 wallpaper.png logo.a8 bpreplay_bold.otf xterm.res gtk3.css gtk4.css gtk-settings.ini firefox-user.js firefox-userChrome.css /opt/optic-de/share/
	install -d /usr/local/share/fonts/optic
	install -m 644 bpreplay_bold.otf /usr/local/share/fonts/optic/
	fc-cache -f >/dev/null 2>&1 || true
	ln -sf /opt/optic-de/bin/optic-settings /usr/local/bin/optic-settings
	ln -sf /opt/optic-de/bin/optic-term /usr/local/bin/optic-term
	ln -sf /opt/optic-de/bin/optic-files /usr/local/bin/optic-files
	ln -sf /opt/optic-de/bin/optic-session /usr/local/bin/optic-session
	ln -sf /opt/optic-de/bin/optic-firefox /usr/local/bin/optic-firefox
	install -d /usr/share/xsessions
	install -m 644 optic.desktop /usr/share/xsessions/optic.desktop

uninstall:
	rm -rf /opt/optic-de /usr/local/share/fonts/optic /usr/share/xsessions/optic.desktop
	cd /usr/local/bin && rm -f optic-settings optic-term optic-files optic-session optic-firefox

clean:
	rm -f optic-de optic-settings optic-term optic-files libsysvshm.so
