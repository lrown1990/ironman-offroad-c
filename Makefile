# Super Off Road in C.
#   make          the library (build/liboffroad.a)
#   make game     build/offroad, the game played (SDL2, Nuked OPL3)
#   make test     the comparisons with the models of OFFROAD CODE OPUS (minutes),
#   make check    the long ones too (half an hour): both only where tests/ exists
CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter
SRC := $(wildcard src/*.c)
OBJ := $(SRC:src/%.c=build/%.o)

all: build/liboffroad.a

build/%.o: src/%.c include/offroad.h
	@mkdir -p build
	$(CC) $(CFLAGS) -fPIC -Iinclude -c $< -o $@

build/liboffroad.a: $(OBJ)
	ar rcs $@ $^

game: build/offroad

build/offroad: sdl/main.c build/liboffroad.a third_party/nuked-opl3/opl3.c
	$(CC) $(CFLAGS) -Iinclude -Ithird_party/nuked-opl3 $$(sdl2-config --cflags) sdl/main.c third_party/nuked-opl3/opl3.c build/liboffroad.a $$(sdl2-config --libs) -o $@

clean:
	rm -rf build

.PHONY: all game clean

-include tests/tests.mk
