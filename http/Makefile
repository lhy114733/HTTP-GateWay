CC       ?= gcc
CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -I.
BUILD    := build

PICO_OBJ    := $(BUILD)/picohttpparser.o
CODEC_TEST  := $(BUILD)/http_codec_test
WRITER_TEST := $(BUILD)/http_response_writer_test
REQUEST_WRITER_TEST := $(BUILD)/http_request_writer_test
RESPONSE_PARSER_TEST := $(BUILD)/http_response_parser_test
EXAMPLE     := $(BUILD)/example

all: $(CODEC_TEST) $(WRITER_TEST) $(REQUEST_WRITER_TEST) $(RESPONSE_PARSER_TEST) $(EXAMPLE)

$(BUILD):
	mkdir -p $(BUILD)

$(PICO_OBJ): picohttpparser.c | $(BUILD)
	$(CC) -std=c99 -O2 -Wall -Wextra -Wno-unused-parameter -c $< -o $@

$(CODEC_TEST): http_codec_test.cc http_message.hpp http_request_parser.hpp $(PICO_OBJ) | $(BUILD)
	$(CXX) $(CXXFLAGS) $< $(PICO_OBJ) -o $@

$(WRITER_TEST): http_response_writer_test.cc http_response_writer.hpp http_message.hpp | $(BUILD)
	$(CXX) $(CXXFLAGS) $< -o $@

$(REQUEST_WRITER_TEST): http_request_writer_test.cc http_request_writer.hpp http_message.hpp | $(BUILD)
	$(CXX) $(CXXFLAGS) $< -o $@

$(RESPONSE_PARSER_TEST): http_response_parser_test.cc http_response_parser.hpp http_message.hpp $(PICO_OBJ) | $(BUILD)
	$(CXX) $(CXXFLAGS) $< $(PICO_OBJ) -o $@

$(EXAMPLE): example.cc $(PICO_OBJ) | $(BUILD)
	$(CXX) $(CXXFLAGS) $< $(PICO_OBJ) -o $@

test: $(CODEC_TEST) $(WRITER_TEST) $(REQUEST_WRITER_TEST) $(RESPONSE_PARSER_TEST)
	./$(CODEC_TEST)
	./$(WRITER_TEST)
	./$(REQUEST_WRITER_TEST)
	./$(RESPONSE_PARSER_TEST)

response-writer-test: $(WRITER_TEST)
	./$(WRITER_TEST)

writer-test: response-writer-test

request-writer-test: $(REQUEST_WRITER_TEST)
	./$(REQUEST_WRITER_TEST)

response-parser-test: $(RESPONSE_PARSER_TEST)
	./$(RESPONSE_PARSER_TEST)

asan: | $(BUILD)
	$(CC) -std=c99 -O1 -g -fsanitize=address,undefined -Wno-unused-parameter -c picohttpparser.c -o $(BUILD)/picohttpparser_asan.o
	$(CXX) -std=c++17 -O1 -g -fsanitize=address,undefined -Wall -Wextra -I. http_codec_test.cc $(BUILD)/picohttpparser_asan.o -o $(BUILD)/http_codec_test_asan
	$(CXX) -std=c++17 -O1 -g -fsanitize=address,undefined -Wall -Wextra -I. http_response_writer_test.cc -o $(BUILD)/http_response_writer_test_asan
	$(CXX) -std=c++17 -O1 -g -fsanitize=address,undefined -Wall -Wextra -I. http_request_writer_test.cc -o $(BUILD)/http_request_writer_test_asan
	$(CXX) -std=c++17 -O1 -g -fsanitize=address,undefined -Wall -Wextra -I. http_response_parser_test.cc $(BUILD)/picohttpparser_asan.o -o $(BUILD)/http_response_parser_test_asan
	./$(BUILD)/http_codec_test_asan
	./$(BUILD)/http_response_writer_test_asan
	./$(BUILD)/http_request_writer_test_asan
	./$(BUILD)/http_response_parser_test_asan

example: $(EXAMPLE)
	./$(EXAMPLE)

clean:
	rm -rf $(BUILD)

.PHONY: all test response-writer-test writer-test request-writer-test response-parser-test asan example clean
