CC = gcc

CFLAGS  = -Wall -Wextra -Wpedantic -std=c11 -O3 -Iinclude -Iexternal/linenoise -Iexternal/sds -Iexternal/cJSON
# -std=c11 alone implies __STRICT_ANSI__, which makes glibc (unlike macOS's
# libc, which doesn't gate on it) hide every POSIX/BSD declaration this
# codebase actually uses beyond strict ISO C11: open_memstream/fsync/fileno
# (db4.c, load.c, wal.c), flock (lock.c - a BSD-origin call, not even POSIX,
# so _POSIX_C_SOURCE alone wouldn't cover it), getpid/chdir/getcwd/opendir/
# readdir/closedir (load.c, main.c), and strncasecmp (lexer.c, main.c).
# _DEFAULT_SOURCE is glibc's documented way to keep -std=c11's strict
# language conformance while restoring its normal (pre-__STRICT_ANSI__)
# library declaration visibility - harmless on macOS, which doesn't
# recognize the macro and already exposes all of the above by default.
CFLAGS += -D_DEFAULT_SOURCE
# value.c's format_double uses log10/floor/fabs/isfinite - libSystem on macOS
# already exposes these without -lm, but glibc needs it linked explicitly.
LDFLAGS = -lm

ifeq ($(OS),Windows_NT)
  TARGET = bin/main.exe
else
  TARGET = bin/main
endif

# arena.c/budget.c/csv.c compile into the binary as scaffolding, even
# though main.c doesn't call them yet - this proves they build clean
# without committing to how they get wired up.
SRC = $(wildcard src/*.c) external/linenoise/linenoise.c external/sds/sds.c external/cJSON/cJSON.c
OBJ = $(SRC:%.c=obj/%.o)

# Everything bin/main links minus main.c itself and linenoise (main.c's own
# REPL line-editing - no other file touches it). This is the public db4.h
# surface plus its real dependencies (sds/cJSON stay internal to csv.c/
# schema.c and never appear in a header db4.h transitively includes), so an
# embedder only needs -Iinclude and this archive - never -Iexternal/*.
LIB_OBJ = $(filter-out obj/src/main.o obj/external/linenoise/%.o,$(OBJ))
LIB_STATIC = bin/libdb4.a

.PHONY: all lib run debug clean

all: $(TARGET)

lib: $(LIB_STATIC)

$(TARGET): $(OBJ)
	mkdir -p bin
	$(CC) $(OBJ) -o $(TARGET) $(LDFLAGS)

$(LIB_STATIC): $(LIB_OBJ)
	mkdir -p bin
	ar rcs $@ $(LIB_OBJ)

# linenoise uses non-standard termios/ioctl calls -Wpedantic flags; needs
# _GNU_SOURCE for some platforms' terminal handling.
obj/external/linenoise/linenoise.o: external/linenoise/linenoise.c
	mkdir -p $(dir $@)
	$(CC) $(filter-out -Wpedantic, $(CFLAGS)) -D_GNU_SOURCE -c $< -o $@

obj/%.o: %.c
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

run: all
	./$(TARGET)

debug: CFLAGS += -g -O0
debug: clean all

clean:
	rm -rf obj bin
