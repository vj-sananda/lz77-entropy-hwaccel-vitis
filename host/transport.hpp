#pragma once
#include "../hls/protocol.hpp"
#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace transport {
inline unsigned word(const unsigned char* p) {
    return unsigned(p[0]) | (unsigned(p[1]) << 8) |
           (unsigned(p[2]) << 16) | (unsigned(p[3]) << 24);
}

// exchange() must send the same complete request and receive the complete
// padded response on every call; short DMA transfers must throw. The pinned
// XDMA driver sets EOP at descriptor-ring wrap even in the middle of a write.
// The FPGA drains that request and returns status 2 with no encoded data.
// Retry it once, after validating this complete rejection, without advancing
// the input, tag, or output. Never retry a partial or ambiguous transaction.
template<class Exchange>
unsigned transfer_frame(Exchange exchange, unsigned char* response,
                        uint64_t block, uint64_t input_offset, unsigned input_size,
                        uint64_t& retries) {
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        exchange();
        const std::string position = " at block " + std::to_string(block) +
            " (input offset " + std::to_string(input_offset) +
            ", size " + std::to_string(input_size) + ")";
        if (word(response) != protocol::RESPONSE_MAGIC || word(response + 8) != uint32_t(block))
            throw std::runtime_error("FPGA response signature or tag mismatch" + position +
                                     "; reprogram the FPGA");
        unsigned size = word(response + 4), status = word(response + 12);
        if (status == 2 && size == 0 && attempt == 0 &&
            std::all_of(response + 16, response + 16 + protocol::CAPACITY,
                        [](unsigned char b) { return b == 0; })) {
            ++retries;
            continue;
        }
        if (status) {
            const char* reason = status == 1 ? "invalid request header" :
                status == 2 ? "invalid AXI byte enables or TLAST" :
                status == 3 ? "invalid LZ token stream" : "unknown status";
            throw std::runtime_error("FPGA status " + std::to_string(status) +
                                     " (" + reason + ")" + position);
        }
        if (size < 13 || size > input_size + 14)
            throw std::runtime_error("FPGA returned invalid frame length " +
                                     std::to_string(size) + position);
        return size;
    }
    throw std::runtime_error("FPGA framing retry exhausted");
}
} // namespace transport
