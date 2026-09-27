# Makefile for libptu.so + libptu-launcher

CC      ?= gcc

# Shared library flags
SO_CFLAGS  := -std=gnu11 -O2 -fPIC -shared \
              -Wall -Wextra -Wno-unused-parameter \
              -ftls-model=global-dynamic \
              -I./include
SO_LDFLAGS := -ldl -lpthread
LTO_FLAGS  := -flto=auto

# Launcher binary flags (normal executable, links libptu.so at runtime via dlopen)
BIN_CFLAGS  := -std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter -I./include
BIN_LDFLAGS := -ldl

SRCS       := src/libptu.c
TARGET     := libptu.so
LAUNCHER   := libptu-launcher
MATERIALIZE := libptu-materialize

.PHONY: all clean debug test install

all: $(TARGET) $(LAUNCHER) $(MATERIALIZE)

$(TARGET): $(SRCS) include/libptu_internal.h
	$(CC) $(SO_CFLAGS) $(LTO_FLAGS) -o $@ $(SRCS) $(SO_LDFLAGS)
	@echo "Built: $@"

$(LAUNCHER): src/libptu-launcher.c include/libptu_internal.h
	$(CC) $(BIN_CFLAGS) -o $@ src/libptu-launcher.c $(BIN_LDFLAGS)
	@echo "Built: $@"

$(MATERIALIZE): src/libptu-materialize.c
	$(CC) $(BIN_CFLAGS) -o $@ src/libptu-materialize.c
	@echo "Built: $@"

# Debug build: AddressSanitizer + debug symbols + verbose logging
debug: $(SRCS) include/libptu_internal.h
	$(CC) $(CFLAGS) -DLIBPTU_DEBUG -g3 -O0 \
		-fsanitize=address,undefined \
		-o libptu_debug.so $(SRCS) $(LDFLAGS)
	@echo "Built: libptu_debug.so"

# Minimal test: load library against /bin/ls in log mode
test: $(TARGET)
	@TMPDIR=$$(mktemp -d) && \
	mkdir -p $$TMPDIR && \
	echo "=== log mode ===" && \
	LIBPTU_MODE=log LIBPTU_OUTPUT=$$TMPDIR \
	    LD_PRELOAD=$(abspath $(TARGET)) \
	    /bin/ls /tmp > /dev/null && \
	echo "Manifest entries: $$(wc -l < $$TMPDIR/cde.manifest)" && \
	head -5 $$TMPDIR/cde.manifest && \
	echo "=== done ===" && \
	rm -rf $$TMPDIR

# Quick overhead check: compare /bin/ls with and without libptu
bench: $(TARGET)
	@echo "=== baseline (no libptu) ===" && \
	TMPDIR=$$(mktemp -d) && \
	time (for i in $$(seq 100); do /bin/ls /usr/lib > /dev/null; done) && \
	echo "=== with libptu log ===" && \
	LIBPTU_MODE=log LIBPTU_OUTPUT=$$TMPDIR \
	    LD_PRELOAD=$(abspath $(TARGET)) \
	    sh -c 'for i in $$(seq 100); do /bin/ls /usr/lib > /dev/null; done' && \
	rm -rf $$TMPDIR

install: $(TARGET)
	install -m 755 $(TARGET) /usr/local/lib/
	ldconfig

clean:
	rm -f $(TARGET) libptu_debug.so $(LAUNCHER) $(MATERIALIZE)
