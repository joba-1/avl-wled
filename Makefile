CXX      ?= g++
CXXFLAGS ?= -O2 -Wall -Wextra -std=c++17
LDLIBS    = -lcurl -lpthread

PREFIX   ?= /usr/local
BINDIR    = $(PREFIX)/bin
SYSCONF  ?= /etc
UNITDIR  ?= /etc/systemd/system

all: avl-wled

avl-wled: avl-wled.cpp
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f avl-wled

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

.PHONY: all clean install uninstall
