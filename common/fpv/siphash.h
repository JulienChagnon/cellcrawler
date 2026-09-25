// SipHash-2-4 (Aumasson & Bernstein). Used as a 64-bit MAC on every UDP packet so random
// traffic hitting the open port can never drive the truck.
#pragma once
#include <cstddef>
#include <cstdint>

namespace fpv {

struct SipKey {
    uint64_t k0 = 0, k1 = 0;
};

namespace detail {
inline uint64_t rotl(uint64_t x, int b) { return (x << b) | (x >> (64 - b)); }
inline uint64_t load64le(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}
inline void sipRound(uint64_t& v0, uint64_t& v1, uint64_t& v2, uint64_t& v3) {
    v0 += v1; v1 = rotl(v1, 13); v1 ^= v0; v0 = rotl(v0, 32);
    v2 += v3; v3 = rotl(v3, 16); v3 ^= v2;
    v0 += v3; v3 = rotl(v3, 21); v3 ^= v0;
    v2 += v1; v1 = rotl(v1, 17); v1 ^= v2; v2 = rotl(v2, 32);
}
}  // namespace detail

inline uint64_t siphash24(const SipKey& key, const uint8_t* data, size_t len) {
    using namespace detail;
    uint64_t v0 = 0x736f6d6570736575ULL ^ key.k0;
    uint64_t v1 = 0x646f72616e646f6dULL ^ key.k1;
    uint64_t v2 = 0x6c7967656e657261ULL ^ key.k0;
    uint64_t v3 = 0x7465646279746573ULL ^ key.k1;

    const size_t full = len & ~static_cast<size_t>(7);
    for (size_t i = 0; i < full; i += 8) {
        uint64_t m = load64le(data + i);
        v3 ^= m;
        sipRound(v0, v1, v2, v3);
        sipRound(v0, v1, v2, v3);
        v0 ^= m;
    }
    uint64_t b = static_cast<uint64_t>(len) << 56;
    for (size_t i = full; i < len; ++i) b |= static_cast<uint64_t>(data[i]) << (8 * (i - full));
    v3 ^= b;
    sipRound(v0, v1, v2, v3);
    sipRound(v0, v1, v2, v3);
    v0 ^= b;
    v2 ^= 0xff;
    for (int i = 0; i < 4; ++i) sipRound(v0, v1, v2, v3);
    return v0 ^ v1 ^ v2 ^ v3;
}

// Parses a 32-hex-character key. Returns false if malformed.
inline bool parseSipKey(const char* hex, SipKey& out) {
    uint8_t bytes[16];
    for (int i = 0; i < 16; ++i) {
        int v = 0;
        for (int j = 0; j < 2; ++j) {
            char c = hex[i * 2 + j];
            int d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else return false;
            v = v * 16 + d;
        }
        bytes[i] = static_cast<uint8_t>(v);
    }
    if (hex[32] != '\0') return false;
    out.k0 = detail::load64le(bytes);
    out.k1 = detail::load64le(bytes + 8);
    return true;
}

}  // namespace fpv
