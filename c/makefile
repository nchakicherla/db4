CC = gcc

CFLAGS  = -Wall -Wextra -Wpedantic -std=c11 -O3 -Iinclude -Iexternal/linenoise -Iexternal/sds -Iexternal/cJSON
LDFLAGS =

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

.PHONY: all run debug clean

all: $(TARGET)

$(TARGET): $(OBJ)
	mkdir -p bin
	$(CC) $(OBJ) -o $(TARGET) $(LDFLAGS)

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
