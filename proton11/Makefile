CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -Iinclude -g -O2
LDFLAGS = -lm

SRC = $(wildcard src/*.c)
OBJ = $(SRC:src/%.c=build/%.o)
BIN = proton

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS)

build/%.o: src/%.c
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf build $(BIN)

.PHONY: all clean
