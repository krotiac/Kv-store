#ifndef KVSTORE_UTILS_CODING_H_
#define KVSTORE_UTILS_CODING_H_

#include <cstdint>
#include <cstring>

namespace kvstore {

// Encodes a 32-bit unsigned integer into a 4-byte buffer in Little-Endian format.
inline void EncodeFixed32(char* buf, uint32_t value) {
    buf[0] = static_cast<char>(value & 0xff);
    buf[1] = static_cast<char>((value >> 8) & 0xff);
    buf[2] = static_cast<char>((value >> 16) & 0xff);
    buf[3] = static_cast<char>((value >> 24) & 0xff);
}

// Decodes a 32-bit unsigned integer from a 4-byte buffer in Little-Endian format.
inline uint32_t DecodeFixed32(const char* ptr) {
    auto uptr = reinterpret_cast<const uint8_t*>(ptr);
    return (static_cast<uint32_t>(uptr[0])) |
           (static_cast<uint32_t>(uptr[1]) << 8) |
           (static_cast<uint32_t>(uptr[2]) << 16) |
           (static_cast<uint32_t>(uptr[3]) << 24);
}

} // namespace kvstore

#endif // KVSTORE_UTILS_CODING_H_
