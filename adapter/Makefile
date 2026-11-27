CC       ?= gcc
CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -pthread -I..
BUILD    := build

PICO_OBJ    := $(BUILD)/picohttpparser.o
SERVER_TEST := $(BUILD)/http_server_test

all: $(SERVER_TEST)

$(BUILD):
	mkdir -p $(BUILD)

$(PICO_OBJ): ../core/http/picohttpparser.c | $(BUILD)
	$(CC) -std=c99 -O2 -Wall -Wextra -Wno-unused-parameter -c $< -o $@

$(SERVER_TEST): ../example/http_server_test.cc ../core/http/http_server.hpp ../core/http/http_server_session.hpp \
		../core/http/http_message.hpp ../core/http/http_request_parser.hpp ../core/http/http_response_writer.hpp \
		../core/net/TcpServer.hpp ../core/net/connection.hpp $(PICO_OBJ) | $(BUILD)
	$(CXX) $(CXXFLAGS) $< $(PICO_OBJ) -o $@

run: $(SERVER_TEST)
	./$(SERVER_TEST) 8080

clean:
	rm -rf $(BUILD)

.PHONY: all run clean
