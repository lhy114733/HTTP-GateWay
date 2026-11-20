CC       ?= gcc
CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -pthread -I..
BUILD    := build

PICO_OBJ  := $(BUILD)/picohttpparser.o
PROXY_TEST := $(BUILD)/http_proxy_test

all: $(PROXY_TEST)

$(BUILD):
	mkdir -p $(BUILD)

$(PICO_OBJ): ../core/http/picohttpparser.c | $(BUILD)
	$(CC) -std=c99 -O2 -Wall -Wextra -Wno-unused-parameter -c $< -o $@

$(PROXY_TEST): ../example/proxy_server_test.cc proxy_session.hpp proxy_manager.hpp ports.hpp \
		../adapter/eventloop_task_runner.hpp ../adapter/host_resolver.hpp ../adapter/http_client_upstream.hpp ../adapter/http_response_sink.hpp \
		../adapter/http_server.hpp ../adapter/http_server_session.hpp ../adapter/http_client_session.hpp \
		../gateway/auth.hpp ../gateway/config.hpp \
		../core/http/http_message.hpp ../core/http/http_request_parser.hpp ../core/http/http_response_writer.hpp \
		../core/http/http_request_writer.hpp ../core/http/http_response_parser.hpp \
		../core/net/TcpServer.hpp ../core/net/TcpClient.hpp ../core/net/eventloop.hpp $(PICO_OBJ) | $(BUILD)
	$(CXX) $(CXXFLAGS) $< $(PICO_OBJ) -o $@

run: $(PROXY_TEST)
	./$(PROXY_TEST) 8081 8080

asan: | $(BUILD)
	$(CC) -std=c99 -O1 -g -fsanitize=address,undefined -Wno-unused-parameter -c ../core/http/picohttpparser.c -o $(BUILD)/picohttpparser_asan.o
	$(CXX) -std=c++17 -O1 -g -fsanitize=address,undefined -Wall -Wextra -pthread -I.. ../example/proxy_server_test.cc $(BUILD)/picohttpparser_asan.o -o $(BUILD)/http_proxy_test_asan

clean:
	rm -rf $(BUILD)

.PHONY: all run asan clean
