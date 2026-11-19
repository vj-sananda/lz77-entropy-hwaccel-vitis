#include "../host/transport.hpp"
#include <cstdio>
#include <vector>

using Response = std::vector<unsigned char>;
static void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
static void put(Response& response, unsigned offset, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) response[offset + i] = value >> (8 * i);
}
static Response reply(unsigned status, unsigned size = 0) {
    Response response(16 + protocol::CAPACITY, 0);
    put(response, 0, protocol::RESPONSE_MAGIC);
    put(response, 4, size);
    put(response, 8, 42);
    put(response, 12, status);
    if (!status && size) response[16] = 0x28;
    return response;
}
static void check(const char* name, const std::vector<Response>& responses,
                  const char* failure, unsigned calls_expected, uint64_t retries_expected,
                  unsigned throw_on_call = 0) {
    Response received(16 + protocol::CAPACITY, 0);
    unsigned calls = 0, size = 0;
    uint64_t retries = 0;
    std::string error;
    try {
        size = transport::transfer_frame([&] {
            ++calls;
            if (calls == throw_on_call) throw std::runtime_error("Short DMA transfer");
            require(calls <= responses.size(), "Unexpected additional DMA exchange");
            std::copy(responses[calls - 1].begin(), responses[calls - 1].end(), received.begin());
        }, received.data(), 42, 1376256, 32768, retries);
    } catch (const std::runtime_error& e) { error = e.what(); }
    require(calls == calls_expected, name);
    require(retries == retries_expected, name);
    if (failure) {
        require(error.find(failure) != std::string::npos, name);
        if (!throw_on_call) require(error.find("block 42 (input offset 1376256, size 32768)") != std::string::npos, name);
    } else {
        require(error.empty() && size == 128, name);
        require(received == responses.back(), "Successful frame must be preserved");
    }
    std::printf("PASS %s\n", name);
}
int main() {
    auto good = reply(0, 128), rejected = reply(2);
    check("valid response", {good}, nullptr, 1, 0);
    check("drained framing rejection retries successfully", {rejected, good}, nullptr, 2, 1);
    check("retry is bounded", {rejected, rejected, good}, "FPGA status 2", 2, 1);
    check("header error is fatal", {reply(1), good}, "FPGA status 1", 1, 0);
    check("LZ error is fatal", {reply(3), good}, "FPGA status 3", 1, 0);
    check("unknown status is fatal", {reply(99), good}, "FPGA status 99", 1, 0);
    auto wrong_magic = rejected; wrong_magic[0] ^= 1;
    check("bad signature is fatal", {wrong_magic, good}, "signature or tag mismatch", 1, 0);
    auto wrong_tag = rejected; put(wrong_tag, 8, 41);
    check("stale tag is fatal", {wrong_tag, good}, "signature or tag mismatch", 1, 0);
    auto dirty_padding = rejected; dirty_padding.back() = 1;
    check("nonzero error payload is fatal", {dirty_padding, good}, "FPGA status 2", 1, 0);
    check("nonempty error frame is fatal", {reply(2, 13), good}, "FPGA status 2", 1, 0);
    check("short valid frame is fatal", {reply(0, 12), good}, "invalid frame length 12", 1, 0);
    check("oversized frame is fatal", {reply(0, 32783), good}, "invalid frame length 32783", 1, 0);
    check("short DMA cannot retry", {rejected, good}, "Short DMA transfer", 1, 0, 1);
    check("short DMA on retry remains fatal", {rejected, good}, "Short DMA transfer", 2, 1, 2);
    check("bad signature on retry remains fatal", {rejected, wrong_magic}, "signature or tag mismatch", 2, 1);
    check("LZ error on retry remains fatal", {rejected, reply(3)}, "FPGA status 3", 2, 1);
    std::puts("TRANSPORT_PASS: 16 cases");
}
