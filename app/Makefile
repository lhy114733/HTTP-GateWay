CC       ?= gcc
CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -pthread -I..
BUILD    := build

PICO_OBJ    := $(BUILD)/picohttpparser.o
SERVER_TEST := $(BUILD)/http_server_test
CLIENT_TEST := $(BUILD)/http_client_test
RESOLVER_TEST := $(BUILD)/host_resolver_test

all: $(SERVER_TEST) $(CLIENT_TEST) $(RESOLVER_TEST)

$(BUILD):
	mkdir -p $(BUILD)

$(PICO_OBJ): ../http/picohttpparser.c | $(BUILD)
	$(CC) -std=c99 -O2 -Wall -Wextra -Wno-unused-parameter -c $< -o $@

$(SERVER_TEST): ../example/http_server_test.cc http_server.hpp http_server_session.hpp \
		../http/http_message.hpp ../http/http_request_parser.hpp ../http/http_response_writer.hpp $(PICO_OBJ) | $(BUILD)
	$(CXX) $(CXXFLAGS) $< $(PICO_OBJ) -o $@

$(CLIENT_TEST): ../example/http_client_test.cc http_client_session.hpp \
		../http/http_message.hpp ../http/http_request_writer.hpp ../http/http_response_parser.hpp $(PICO_OBJ) | $(BUILD)
	$(CXX) $(CXXFLAGS) $< $(PICO_OBJ) -o $@

$(RESOLVER_TEST): host_resolver_test.cc host_resolver.hpp | $(BUILD)
	$(CXX) $(CXXFLAGS) $< -o $@

run: $(SERVER_TEST)
	./$(SERVER_TEST) 8080

resolver-test: $(RESOLVER_TEST)
	./$(RESOLVER_TEST)

asan: | $(BUILD)
	$(CC) -std=c99 -O1 -g -fsanitize=address,undefined -Wno-unused-parameter -c ../http/picohttpparser.c -o $(BUILD)/picohttpparser_asan.o
	$(CXX) -std=c++17 -O1 -g -fsanitize=address,undefined -Wall -Wextra -pthread -I.. ../example/http_server_test.cc $(BUILD)/picohttpparser_asan.o -o $(BUILD)/http_server_test_asan
	$(CXX) -std=c++17 -O1 -g -fsanitize=address,undefined -Wall -Wextra -pthread -I.. ../example/http_client_test.cc $(BUILD)/picohttpparser_asan.o -o $(BUILD)/http_client_test_asan
	$(CXX) -std=c++17 -O1 -g -fsanitize=address,undefined -Wall -Wextra -pthread -I.. host_resolver_test.cc -o $(BUILD)/host_resolver_test_asan

clean:
	rm -rf $(BUILD)

.PHONY: all run resolver-test asan clean
