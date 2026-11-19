#pragma once
// All wire words are little endian. Each transaction is fixed size, including
// padding, so XDMA transfers do not depend on compressed frame length.
namespace protocol {
constexpr unsigned BLOCK = 32768;
constexpr unsigned CAPACITY = 65536;
constexpr unsigned REQUEST_MAGIC = 0x3354535a; // ZST3
constexpr unsigned RESPONSE_MAGIC = 0x3352535a; // ZSR3
constexpr unsigned DEVICE_ID = 0x5a534533; // ZSE3; incompatible with LZ-only shell
}
