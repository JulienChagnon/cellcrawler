// Consistent Overhead Byte Stuffing. Encoded frames contain no 0x00 bytes, so 0x00 is
// used as the frame delimiter on the serial link. A reader that joins mid-stream
// resynchronises at the next 0x00.
#pragma once
#include <cstddef>
#include <cstdint>

namespace fpv {

// Worst case output size for an input of n bytes (excluding the trailing delimiter).
constexpr size_t cobsMaxEncoded(size_t n) { return n + n / 254 + 1; }

// Encodes src into dst (dst must hold cobsMaxEncoded(len) bytes). Returns encoded length.
inline size_t cobsEncode(const uint8_t* src, size_t len, uint8_t* dst) {
    size_t out = 1, codeIdx = 0;
    uint8_t code = 1;
    for (size_t i = 0; i < len; ++i) {
        if (src[i] == 0) {
            dst[codeIdx] = code;
            codeIdx = out++;
            code = 1;
        } else {
            dst[out++] = src[i];
            if (++code == 0xFF) {
                dst[codeIdx] = code;
                codeIdx = out++;
                code = 1;
            }
        }
    }
    dst[codeIdx] = code;
    return out;
}

// Decodes src (without delimiter) into dst. Returns decoded length, or 0 on malformed input.
inline size_t cobsDecode(const uint8_t* src, size_t len, uint8_t* dst) {
    size_t in = 0, out = 0;
    while (in < len) {
        uint8_t code = src[in++];
        if (code == 0 || in + code - 1 > len) return 0;
        for (uint8_t i = 1; i < code; ++i) dst[out++] = src[in++];
        if (code != 0xFF && in < len) dst[out++] = 0;
    }
    return out;
}

}  // namespace fpv
