CC = gcc
CSTD = -std=c2x
CFLAGS = $(CSTD) -Wall -Wextra -Wpedantic -g -Iinclude
CFLAGS += $(shell pkg-config --cflags ncursesw sqlite3)
LDFLAGS = $(shell pkg-config --libs ncursesw sqlite3)

SRC = $(wildcard src/*.c) $(wildcard src/**/*.c)
OBJ = $(patsubst src/%.c,build/%.o,$(SRC))
BIN = ficli

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(OBJ) -o $@ $(LDFLAGS)

build/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

run: $(BIN)
	./$(BIN)

clean:
	rm -rf build $(BIN)

.PHONY: all clean run
