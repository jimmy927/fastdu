CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra
VERSION ?= dev
PREFIX ?= /usr/local

fastdu: src/unix/fastdu.c src/options.h
	$(CC) $(CFLAGS) -pthread -DFASTDU_VERSION=$(VERSION) -o $@ src/unix/fastdu.c $(LDFLAGS)

test: fastdu
	./test/unix.sh ./fastdu

install: fastdu
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	install -m 755 fastdu $(DESTDIR)$(PREFIX)/bin/fastdu

clean:
	rm -f fastdu

.PHONY: test install clean
