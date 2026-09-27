CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra
VERSION ?= dev
PREFIX ?= /usr/local

fastdu: src/linux/fastdu.c src/options.h
	$(CC) $(CFLAGS) -pthread -DFASTDU_VERSION=$(VERSION) -o $@ src/linux/fastdu.c $(LDFLAGS)

test: fastdu
	./test/linux.sh ./fastdu

install: fastdu
	install -Dm755 fastdu $(DESTDIR)$(PREFIX)/bin/fastdu

clean:
	rm -f fastdu

.PHONY: test install clean
