CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -pthread -I..
BUILD    := build

AUTH_TEST := $(BUILD)/auth_test

all: $(AUTH_TEST)

$(BUILD):
	mkdir -p $(BUILD)

$(AUTH_TEST): auth_test.cc auth.hpp config.hpp ../http/http_message.hpp | $(BUILD)
	$(CXX) $(CXXFLAGS) $< -o $@

unit-test: $(AUTH_TEST)
	./$(AUTH_TEST)

asan: | $(BUILD)
	$(CXX) -std=c++17 -O1 -g -fsanitize=address,undefined -Wall -Wextra -pthread -I.. \
		auth_test.cc -o $(BUILD)/auth_test_asan
	./$(BUILD)/auth_test_asan

clean:
	rm -rf $(BUILD)

.PHONY: all unit-test asan clean
