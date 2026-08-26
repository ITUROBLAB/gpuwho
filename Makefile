## gpuwho -- who is using the GPUs right now

CC        ?= cc
PREFIX    ?= /usr/local
DESTDIR   ?=
CUDA_HOME ?= /usr/local/cuda

BINDIR  := $(DESTDIR)$(PREFIX)/bin
UNITDIR := $(DESTDIR)/etc/systemd/system
CONFDIR := $(DESTDIR)/etc/gpuwho

VERSION := 0.1.1
DEB_MAINTAINER ?= Kadir Yavuz Kurt <k.yavuzkurt1@gmail.com>
DEB_ARCH := $(shell dpkg --print-architecture 2>/dev/null || echo amd64)
DEB_NAME := gpuwho_$(VERSION)_$(DEB_ARCH).deb
# Deferred (=, not :=): OBJDIR is defined further down.
DEBROOT   = $(OBJDIR)/deb

BIN    := gpuwho
SRCDIR := src
OBJDIR := build

SRCS := $(sort $(wildcard $(SRCDIR)/*.c))
OBJS := $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(SRCS))
DEPS := $(OBJS:.o=.d)

# --- locate nvml.h -----------------------------------------------------------
# The driver ships libnvidia-ml; the header comes with either the driver
# development package or the CUDA toolkit.
NVML_H := $(firstword $(wildcard \
	/usr/include/nvml.h \
	$(CUDA_HOME)/include/nvml.h \
	/usr/local/cuda-*/targets/*/include/nvml.h \
	/opt/cuda/include/nvml.h))

ifeq ($(NVML_H),)
$(error nvml.h not found. Install the CUDA toolkit headers (nvidia-cuda-dev / \
cuda-nvml-dev) or set CUDA_HOME=/path/to/cuda)
endif

NVML_CFLAGS := -I$(patsubst %/,%,$(dir $(NVML_H)))

# --- locate a link target for libnvidia-ml -----------------------------------
# Preference order: a plain -lnvidia-ml (a libnvidia-ml.so symlink exists), the
# CUDA stub library (records the correct SONAME, resolves to the real driver
# library at run time), then the versioned driver library by full path.
# libnvidia-ml must match the running driver, so it is never linked statically.
NVML_LIBS := $(shell \
	if printf 'int main(void){return 0;}' | $(CC) -x c - -o /dev/null -lnvidia-ml >/dev/null 2>&1; then \
		echo '-lnvidia-ml'; \
	else \
		for d in $(CUDA_HOME)/lib64/stubs /usr/lib/x86_64-linux-gnu /usr/lib64 /usr/lib; do \
			if [ -e "$$d/libnvidia-ml.so" ]; then echo "-L$$d -lnvidia-ml"; exit 0; fi; \
		done; \
		for d in /usr/lib/x86_64-linux-gnu /lib/x86_64-linux-gnu /usr/lib64 /usr/lib; do \
			if [ -e "$$d/libnvidia-ml.so.1" ]; then echo "$$d/libnvidia-ml.so.1"; exit 0; fi; \
		done; \
		echo '-lnvidia-ml'; \
	fi)

WARN := -Wall -Wextra -Wshadow -Wpointer-arith -Wstrict-prototypes \
        -Wmissing-prototypes -Wno-unused-parameter

CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -D_GNU_SOURCE $(WARN) $(NVML_CFLAGS)
CFLAGS  += -DGPUWHO_VERSION='"$(VERSION)"'
LDLIBS  += $(NVML_LIBS)

# Setting CFLAGS on the command line replaces the whole variable, which would
# drop the include path above.  Use these to add flags instead:
#   make EXTRA_CFLAGS=-fsanitize=address EXTRA_LDFLAGS=-fsanitize=address
CFLAGS  += $(EXTRA_CFLAGS)
LDFLAGS += $(EXTRA_LDFLAGS)

.PHONY: all clean install uninstall install-units install-conf config test deb printversion

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(OBJDIR)/%.o: $(SRCDIR)/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

$(OBJDIR):
	@mkdir -p $(OBJDIR)

# Single source of truth for the version, so CI can check it against the tag.
printversion:
	@echo $(VERSION)

config:
	@echo "version     = $(VERSION)"
	@echo "CC          = $(CC)"
	@echo "nvml.h      = $(NVML_H)"
	@echo "NVML_CFLAGS = $(NVML_CFLAGS)"
	@echo "NVML_LIBS   = $(NVML_LIBS)"

test: $(BIN)
	@sh tests/run.sh ./$(BIN)

clean:
	rm -rf $(OBJDIR) $(BIN)

# The man page carries the version in its .TH line; stamp it from VERSION so
# there is still only one place to bump.
$(OBJDIR)/gpuwho.1: doc/gpuwho.1 Makefile | $(OBJDIR)
	sed -e 's|"gpuwho [0-9][0-9.]*"|"gpuwho $(VERSION)"|' $< > $@

install: $(BIN) $(OBJDIR)/gpuwho.1
	install -d $(BINDIR)
	install -m 0755 $(BIN) $(BINDIR)/$(BIN)
	install -d $(DESTDIR)$(PREFIX)/share/man/man1
	gzip -9nc $(OBJDIR)/gpuwho.1 > $(DESTDIR)$(PREFIX)/share/man/man1/gpuwho.1.gz
	chmod 0644 $(DESTDIR)$(PREFIX)/share/man/man1/gpuwho.1.gz

# The units reference gpuwho by absolute path, so the path is substituted at
# install time rather than duplicated between the repo and the package.
$(OBJDIR)/%.service: systemd/%.service.in | $(OBJDIR)
	sed -e 's|@BINDIR@|$(PREFIX)/bin|g' $< > $@

install-units: $(OBJDIR)/gpuwho-accounting.service $(OBJDIR)/gpuwho-collect.service
	install -d $(UNITDIR)
	install -m 0644 $(OBJDIR)/gpuwho-accounting.service $(UNITDIR)/
	install -m 0644 $(OBJDIR)/gpuwho-collect.service    $(UNITDIR)/
	install -m 0644 systemd/gpuwho-collect.timer        $(UNITDIR)/
	@echo
	@echo "Now run:"
	@echo "  systemctl daemon-reload"
	@echo "  systemctl enable --now gpuwho-accounting.service"
	@echo "  systemctl enable --now gpuwho-collect.timer"

install-conf:
	install -d $(CONFDIR)
	test -e $(CONFDIR)/ignore.conf || install -m 0644 conf/ignore.conf $(CONFDIR)/ignore.conf

uninstall:
	rm -f $(BINDIR)/$(BIN)
	rm -f $(DESTDIR)$(PREFIX)/share/man/man1/gpuwho.1.gz
	rm -f $(UNITDIR)/gpuwho-accounting.service
	rm -f $(UNITDIR)/gpuwho-collect.service
	rm -f $(UNITDIR)/gpuwho-collect.timer

# --- .deb ---------------------------------------------------------------------
# Built with dpkg-deb directly: no debhelper, no build-deps beyond dpkg-deb and
# fakeroot.  The postinst enables accounting and starts the timer, so the
# machine is ready to report as soon as the package is installed.
deb: $(BIN) $(OBJDIR)/gpuwho.1 $(OBJDIR)/gpuwho-accounting.service $(OBJDIR)/gpuwho-collect.service
	@command -v dpkg-deb >/dev/null || { echo "dpkg-deb not found"; exit 1; }
	@case '$(DEBROOT)' in $(OBJDIR)/*) ;; *) \
		echo "refusing to clean '$(DEBROOT)': not under $(OBJDIR)"; exit 1 ;; esac
	rm -rf $(DEBROOT)
	install -d $(DEBROOT)/DEBIAN
	install -d $(DEBROOT)/usr/bin
	install -d $(DEBROOT)/usr/lib/systemd/system
	install -d $(DEBROOT)/etc/gpuwho
	install -d $(DEBROOT)/usr/share/doc/gpuwho
	install -d $(DEBROOT)/usr/share/man/man1
	install -m 0755 $(BIN) $(DEBROOT)/usr/bin/gpuwho
	gzip -9nc $(OBJDIR)/gpuwho.1 > $(DEBROOT)/usr/share/man/man1/gpuwho.1.gz
	chmod 0644 $(DEBROOT)/usr/share/man/man1/gpuwho.1.gz
	sed -e 's|@BINDIR@|/usr/bin|g' systemd/gpuwho-accounting.service.in \
		> $(DEBROOT)/usr/lib/systemd/system/gpuwho-accounting.service
	sed -e 's|@BINDIR@|/usr/bin|g' systemd/gpuwho-collect.service.in \
		> $(DEBROOT)/usr/lib/systemd/system/gpuwho-collect.service
	install -m 0644 systemd/gpuwho-collect.timer \
		$(DEBROOT)/usr/lib/systemd/system/gpuwho-collect.timer
	chmod 0644 $(DEBROOT)/usr/lib/systemd/system/*.service
	install -m 0644 conf/ignore.conf $(DEBROOT)/etc/gpuwho/ignore.conf
	install -m 0644 README.md       $(DEBROOT)/usr/share/doc/gpuwho/README.md
	install -m 0644 packaging/copyright $(DEBROOT)/usr/share/doc/gpuwho/copyright
	printf 'gpuwho (%s) unstable; urgency=medium\n\n  * Initial release.\n\n -- %s  %s\n' \
		'$(VERSION)' '$(DEB_MAINTAINER)' "$$(date -R)" \
		| gzip -9n > $(DEBROOT)/usr/share/doc/gpuwho/changelog.Debian.gz
	chmod 0644 $(DEBROOT)/usr/share/doc/gpuwho/changelog.Debian.gz
	echo /etc/gpuwho/ignore.conf > $(DEBROOT)/DEBIAN/conffiles
	install -m 0755 packaging/postinst $(DEBROOT)/DEBIAN/postinst
	install -m 0755 packaging/prerm    $(DEBROOT)/DEBIAN/prerm
	install -m 0755 packaging/postrm   $(DEBROOT)/DEBIAN/postrm
	sed -e 's|@VERSION@|$(VERSION)|g' \
	    -e 's|@ARCH@|$(DEB_ARCH)|g' \
	    -e 's|@MAINTAINER@|$(DEB_MAINTAINER)|g' \
	    -e "s|@INSTALLED_SIZE@|$$(du -ks $(DEBROOT) | cut -f1)|g" \
	    packaging/control.in > $(DEBROOT)/DEBIAN/control
	cd $(DEBROOT) && find . -path ./DEBIAN -prune -o -type f -printf '%P\0' \
		| xargs -0 md5sum > DEBIAN/md5sums
	fakeroot dpkg-deb --build $(DEBROOT) $(DEB_NAME)
	@echo
	@dpkg-deb --info $(DEB_NAME) | sed -n '1,20p'
	@echo "built $(DEB_NAME)"

-include $(DEPS)
