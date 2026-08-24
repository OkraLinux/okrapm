# Makefile for oaatools - install / uninstall the OAA toolkit system-wide

PREFIX     ?= /usr
BINDIR      = $(DESTDIR)$(PREFIX)/bin
LIBDIR      = $(DESTDIR)$(PREFIX)/lib/okrapm

SCRIPTS    = oaa-new oaa-build oaa-inspect oaa-extract oaa-list oaa-verify oaatools
LIBFILES   = oaatools-common.sh

.PHONY: all install uninstall clean test

all:
	@echo "Nothing to build (pure shell toolkit)."

install:
	@mkdir -p $(BINDIR) $(LIBDIR)
	@for f in $(SCRIPTS); do \
	    install -m 0755 $$f $(BINDIR)/$$f; \
	    echo "installed $$f -> $(BINDIR)/$$f"; \
	done
	@for f in $(LIBFILES); do \
	    install -m 0644 $$f $(LIBDIR)/$$f; \
	    echo "installed $$f -> $(LIBDIR)/$$f"; \
	done
	@ln -sf $(BINDIR)/oaatools $(BINDIR)/oaa
	@echo "symlinked $(BINDIR)/oaa -> oaatools"

uninstall:
	@for f in $(SCRIPTS) oaa; do rm -f $(BINDIR)/$$f; done
	@rm -rf $(LIBDIR)
	@echo "removed oaatools from $(BINDIR) and $(LIBDIR)"

test:
	@./oaatools version
	@./oaatools help >/dev/null && echo "help: ok"

clean:
	@find . -name '*.oaa' -o -name '*.oaa.sha256' | xargs -r rm -f
