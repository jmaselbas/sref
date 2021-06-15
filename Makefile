# See LICENSE file for copyright and license details.
.POSIX:

include config.mk

SRC = sref.c stbi.c glad.c
BIN = sref
OBJ = $(SRC:.c=.o)

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS) $(LDLIBS)

$(OBJ): config.mk

install: all
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	cp -f $(BIN) $(DESTDIR)$(PREFIX)/bin
	chmod 755 $(DESTDIR)$(PREFIX)/bin/$(BIN)
	mkdir -p $(DESTDIR)$(MANPREFIX)/man1
	cp -f $(BIN).1 $(DESTDIR)$(MANPREFIX)/man1

uninstall:
	rm -vf $(DESTDIR)$(PREFIX)/bin/$(BIN)
	rm -vf $(DESTDIR)$(MANPREFIX)/man1/$(BIN).1

dist:
	mkdir -p $(BIN)-$(VERSION)
	cp $(DISTFILES) $(BIN)-$(VERSION)
	tar -cf $(BIN)-$(VERSION).tar $(BIN)-$(VERSION)
	gzip $(BIN)-$(VERSION).tar
	rm -rf $(BIN)-$(VERSION)

clean:
	rm -f $(BIN) $(OBJ) $(BIN)-$(VERSION).tar.gz

.PHONY: all install uninstall dist clean
