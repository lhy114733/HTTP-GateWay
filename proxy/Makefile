CC       ?= gcc
CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -pthread -I..
BUILD    := build

PICO_OBJ  := $(BUILD)/picohttpparser.o
PROXY_TEST := $(BUILD)/http_proxy_test
UNIT_TEST  := $(BUILD)/proxy_session_test

all: $(PROXY_TEST) $(UNIT_TEST)

$(BUILD):
	mkdir -p $(BUILD)

$(PICO_OBJ): ../http/picohttpparser.c | $(BUILD)
	$(CC) -std=c99 -O2 -Wall -Wextra -Wno-unused-parameter -c $< -o $@

$(PROXY_TEST): ../example/proxy_server_test.cc proxy_session.hpp proxy_manager.hpp ports.hpp \
		../app/eventloop_task_runner.hpp ../app/host_resolver.hpp ../app/http_client_upstream.hpp ../app/http_response_sink.hpp \
		../app/http_server.hpp ../app/http_server_session.hpp ../app/http_client_session.hpp \
		../gateway/auth.hpp ../gateway/config.hpp \
		../http/http_message.hpp ../http/http_request_parser.hpp ../http/http_response_writer.hpp \
		../http/http_request_writer.hpp ../http/http_response_parser.hpp $(PICO_OBJ) | $(BUILD)
	$(CXX) $(CXXFLAGS) $< $(PICO_OBJ) -o $@

$(UNIT_TEST): proxy_session_test.cc proxy_session.hpp ports.hpp ../http/http_message.hpp | $(BUILD)
	$(CXX) $(CXXFLAGS) $< -o $@

run: $(PROXY_TEST)
	./$(PROXY_TEST) 8081 8080

unit-test: $(UNIT_TEST)
	./$(UNIT_TEST)

asan: | $(BUILD)
	$(CC) -std=c99 -O1 -g -fsanitize=address,undefined -Wno-unused-parameter -c ../http/picohttpparser.c -o $(BUILD)/picohttpparser_asan.o
	$(CXX) -std=c++17 -O1 -g -fsanitize=address,undefined -Wall -Wextra -pthread -I.. ../example/proxy_server_test.cc $(BUILD)/picohttpparser_asan.o -o $(BUILD)/http_proxy_test_asan
	$(CXX) -std=c++17 -O1 -g -fsanitize=address,undefined -Wall -Wextra -pthread -I.. proxy_session_test.cc -o $(BUILD)/proxy_session_test_asan

clean:
	rm -rf $(BUILD)

.PHONY: all run unit-test asan clean
