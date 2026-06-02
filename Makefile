CXX      ?= g++
CXXFLAGS ?= -O2 -Wall -Wextra -std=c++17
LDLIBS    = -lcurl -lpthread

PREFIX   ?= /usr/local
BINDIR    = $(PREFIX)/bin
SYSCONF  ?= /etc
UNITDIR  ?= /etc/systemd/system

all: avl-wled

avl-wled: avl-wled.cpp core.h apple_touch_icon.h
	$(CXX) $(CXXFLAGS) -o $@ avl-wled.cpp $(LDLIBS)

# Embed the homescreen icon. Regenerating apple-touch-icon.png from logo.svg
# requires inkscape; the PNG is checked in so normal builds don't need it.
apple_touch_icon.h: apple-touch-icon.png
	xxd -i apple-touch-icon.png > $@

apple-touch-icon.png: logo.svg
	inkscape -w 180 -h 180 $< -o $@

tests/unit: tests/unit.cpp tests/doctest.h core.h
	$(CXX) $(CXXFLAGS) -I tests -o $@ tests/unit.cpp

test: tests/unit
	./tests/unit

clean:
	rm -f avl-wled tests/unit

install: avl-wled
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 avl-wled $(DESTDIR)$(BINDIR)/avl-wled
	install -d $(DESTDIR)$(SYSCONF)
	[ -f $(DESTDIR)$(SYSCONF)/avl-wled.conf ] || \
	    install -m 0644 avl-wled.conf $(DESTDIR)$(SYSCONF)/avl-wled.conf
	install -d $(DESTDIR)$(UNITDIR)
	install -m 0644 avl-wled.service $(DESTDIR)$(UNITDIR)/avl-wled.service

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/avl-wled
	rm -f $(DESTDIR)$(UNITDIR)/avl-wled.service

.PHONY: all clean install uninstall test
