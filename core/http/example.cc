// picohttpparser 最小示例：
//   1. 请求解析（Content-Length body），先喂半包再喂完整包
//   2. 响应解析（chunked body），用 phr_decode_chunked 去 chunk
//
// 编译（在项目根目录）：
//   gcc -O2 -c core/http/picohttpparser.c -o core/http/picohttpparser.o
//   g++ -std=c++17 -O2 -I core/http core/http/example.cc core/http/picohttpparser.o -o core/http/example
//   ./core/http/example

#include "picohttpparser.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

struct Header
{
    std::string name;
    std::string value;
};

struct Request
{
    std::string method;
    std::string path;
    int minor_version = 0;
    std::vector<Header> headers;
    std::string body;
};

struct Response
{
    int status = 0;
    std::string reason;
    int minor_version = 0;
    std::vector<Header> headers;
    std::string body;
};

enum class ParseResult
{
    Complete,
    NeedMore,
    Error
};

static std::string ToLower(std::string s)
{
    for (char& c : s)
    {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

static const Header* FindHeader(const std::vector<Header>& headers, const char* name)
{
    const std::string want = ToLower(name);
    for (const Header& h : headers)
    {
        if (ToLower(h.name) == want) return &h;
    }
    return nullptr;
}

static size_t ParseContentLength(const Header* h)
{
    if (h == nullptr) return 0;
    return static_cast<size_t>(std::strtoull(h->value.c_str(), nullptr, 10));
}

// 返回 Complete 时，consumed 是整个消息（header + body）的字节数。
static ParseResult ParseRequest(const char* data, size_t len, Request& out, size_t& consumed)
{
    const char* method = nullptr;
    const char* path = nullptr;
    size_t method_len = 0;
    size_t path_len = 0;
    int minor_version = 0;

    phr_header headers[64];
    size_t num_headers = sizeof(headers) / sizeof(headers[0]);

    // last_len 传 0 表示每次从头解析；请求量不大时最简单。
    int ret = phr_parse_request(data, len, &method, &method_len, &path, &path_len,
                                &minor_version, headers, &num_headers, 0);
    if (ret == -2) return ParseResult::NeedMore;
    if (ret == -1) return ParseResult::Error;

    out.method.assign(method, method_len);
    out.path.assign(path, path_len);
    out.minor_version = minor_version;
    out.headers.clear();
    for (size_t i = 0; i < num_headers; ++i)
    {
        out.headers.push_back(Header{
            std::string(headers[i].name, headers[i].name_len),
            std::string(headers[i].value, headers[i].value_len)});
    }

    // pico 只解析 header；body 的边界由 Content-Length 决定。
    // Transfer-Encoding: chunked 的请求这里没处理，先用 Content-Length 版本。
    const size_t body_len = ParseContentLength(FindHeader(out.headers, "content-length"));
    if (len < static_cast<size_t>(ret) + body_len) return ParseResult::NeedMore;

    out.body.assign(data + ret, body_len);
    consumed = static_cast<size_t>(ret) + body_len;
    return ParseResult::Complete;
}

// 返回 Complete 时，consumed 是整个消息（header + body）的字节数。
static ParseResult ParseResponse(const char* data, size_t len, Response& out, size_t& consumed)
{
    int minor_version = 0;
    int status = 0;
    const char* msg = nullptr;
    size_t msg_len = 0;

    phr_header headers[64];
    size_t num_headers = sizeof(headers) / sizeof(headers[0]);

    int ret = phr_parse_response(data, len, &minor_version, &status, &msg, &msg_len,
                                 headers, &num_headers, 0);
    if (ret == -2) return ParseResult::NeedMore;
    if (ret == -1) return ParseResult::Error;

    out.status = status;
    out.reason.assign(msg, msg_len);
    out.minor_version = minor_version;
    out.headers.clear();
    for (size_t i = 0; i < num_headers; ++i)
    {
        out.headers.push_back(Header{
            std::string(headers[i].name, headers[i].name_len),
            std::string(headers[i].value, headers[i].value_len)});
    }

    const Header* te = FindHeader(out.headers, "transfer-encoding");
    const bool chunked = te != nullptr &&
                         ToLower(te->value).find("chunked") != std::string::npos;

    if (chunked)
    {
        const size_t encoded_len = len - static_cast<size_t>(ret);
        if (encoded_len == 0) return ParseResult::NeedMore;
        std::string tmp(data + ret, encoded_len); // decoder 会原地改写
        phr_chunked_decoder decoder{};
        decoder.consume_trailer = 1; // 吃掉 0\r\n 后面的 trailer/CRLF
        size_t decoded_len = tmp.size();
        ssize_t left = phr_decode_chunked(&decoder, &tmp[0], &decoded_len);
        if (left == -2) return ParseResult::NeedMore;
        if (left < 0) return ParseResult::Error;

        out.body.assign(tmp.data(), decoded_len);
        consumed = static_cast<size_t>(ret) + encoded_len - static_cast<size_t>(left);
        return ParseResult::Complete;
    }

    const size_t body_len = ParseContentLength(FindHeader(out.headers, "content-length"));
    if (len < static_cast<size_t>(ret) + body_len) return ParseResult::NeedMore;

    out.body.assign(data + ret, body_len);
    consumed = static_cast<size_t>(ret) + body_len;
    return ParseResult::Complete;
}

int main()
{
    int failed = 0;

    const std::string raw_request =
        "POST /v1/chat/completions HTTP/1.1\r\n"
        "Host: gateway.local\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 13\r\n"
        "\r\n"
        "{\"model\":\"x\"}";

    Request req;
    size_t consumed = 0;

    // 先喂 20 字节，模拟半包
    ParseResult r1 = ParseRequest(raw_request.data(), 20, req, consumed);
    ParseResult r2 = ParseRequest(raw_request.data(), raw_request.size(), req, consumed);

    if (r1 != ParseResult::NeedMore || r2 != ParseResult::Complete ||
        req.method != "POST" || req.path != "/v1/chat/completions" ||
        req.body != "{\"model\":\"x\"}" || consumed != raw_request.size())
    {
        std::printf("[FAIL] request: r1=%d r2=%d method=%s path=%s body=%s consumed=%zu\n",
                    static_cast<int>(r1), static_cast<int>(r2),
                    req.method.c_str(), req.path.c_str(), req.body.c_str(), consumed);
        ++failed;
    }
    else
    {
        std::printf("[ OK ] request: %s %s, body=%s\n",
                    req.method.c_str(), req.path.c_str(), req.body.c_str());
    }

    const std::string raw_response =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "7\r\n"
        "data: x\r\n"
        "0\r\n"
        "\r\n";

    Response resp;
    consumed = 0;
    ParseResult rr = ParseResponse(raw_response.data(), raw_response.size(), resp, consumed);

    if (rr != ParseResult::Complete || resp.status != 200 ||
        resp.body != "data: x" || consumed != raw_response.size())
    {
        std::printf("[FAIL] response: rr=%d status=%d body=\"%s\" consumed=%zu\n",
                    static_cast<int>(rr), resp.status, resp.body.c_str(), consumed);
        ++failed;
    }
    else
    {
        std::printf("[ OK ] response: %d, chunked body=\"%s\"\n",
                    resp.status, resp.body.c_str());
    }

    std::printf("\n%s\n", failed == 0 ? "PASS" : "FAIL");
    return failed == 0 ? 0 : 1;
}
