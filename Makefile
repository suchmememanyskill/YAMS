# YAMS — Makefile

CC      = gcc
CFLAGS  = -std=c11 -D_GNU_SOURCE -Wall -Wextra -Wpedantic -O2
TARGET  = yams
SRC     = yams.c

PREFIX  ?= /usr/local
BINDIR  = $(PREFIX)/bin

.PHONY: all clean install uninstall

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $<

install: $(TARGET)
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET)

clean:
	rm -f $(TARGET)
