## gpuwho -- who is using the GPUs right now

CC        ?= cc
PREFIX    ?= /usr/local
DESTDIR   ?=
CUDA_HOME ?= /usr/local/cuda

BINDIR  := $(DESTDIR)$(PREFIX)/bin
UNITDIR := $(DESTDIR)/etc/systemd/system

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
LDLIBS  += $(NVML_LIBS)

# Setting CFLAGS on the command line replaces the whole variable, which would
# drop the include path above.  Use these to add flags instead:
#   make EXTRA_CFLAGS=-fsanitize=address EXTRA_LDFLAGS=-fsanitize=address
CFLAGS  += $(EXTRA_CFLAGS)
LDFLAGS += $(EXTRA_LDFLAGS)

.PHONY: all clean install uninstall install-units config test

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(OBJDIR)/%.o: $(SRCDIR)/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

$(OBJDIR):
	@mkdir -p $(OBJDIR)

config:
	@echo "CC          = $(CC)"
	@echo "nvml.h      = $(NVML_H)"
	@echo "NVML_CFLAGS = $(NVML_CFLAGS)"
	@echo "NVML_LIBS   = $(NVML_LIBS)"

test: $(BIN)
	@sh tests/run.sh ./$(BIN)

clean:
	rm -rf $(OBJDIR) $(BIN)

install: $(BIN)
	install -d $(BINDIR)
	install -m 0755 $(BIN) $(BINDIR)/$(BIN)

install-units:
	install -d $(UNITDIR)
	install -m 0644 systemd/gpuwho-accounting.service $(UNITDIR)/
	install -m 0644 systemd/gpuwho-collect.service    $(UNITDIR)/
	install -m 0644 systemd/gpuwho-collect.timer      $(UNITDIR)/
	@echo
	@echo "Now run:"
	@echo "  systemctl daemon-reload"
	@echo "  systemctl enable --now gpuwho-accounting.service"
	@echo "  systemctl enable --now gpuwho-collect.timer"

uninstall:
	rm -f $(BINDIR)/$(BIN)
	rm -f $(UNITDIR)/gpuwho-accounting.service
	rm -f $(UNITDIR)/gpuwho-collect.service
	rm -f $(UNITDIR)/gpuwho-collect.timer

-include $(DEPS)
