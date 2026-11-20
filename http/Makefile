CC       ?= gcc
CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -I.
BUILD    := build

PICO_OBJ := $(BUILD)/picohttpparser.o
EXAMPLE  := $(BUILD)/example

all: $(EXAMPLE)

$(BUILD):
	mkdir -p $(BUILD)

$(PICO_OBJ): picohttpparser.c | $(BUILD)
	$(CC) -std=c99 -O2 -Wall -Wextra -Wno-unused-parameter -c $< -o $@

$(EXAMPLE): example.cc $(PICO_OBJ) | $(BUILD)
	$(CXX) $(CXXFLAGS) $< $(PICO_OBJ) -o $@

example: $(EXAMPLE)
	./$(EXAMPLE)

clean:
	rm -rf $(BUILD)

.PHONY: all example clean
