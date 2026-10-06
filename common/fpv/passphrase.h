// Shared secret -> SipHash key. The secret is either a 32-hex-character key (fpv-pc --genkey)
// or a passphrase, which is stretched with PBKDF2-HMAC-SHA256 so that guessing it from
// captured packets costs ~100k hashes per guess. PC and phone both run this, so the same
// text typed on each gives the same key.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "siphash.h"

namespace fpv {

namespace sha256 {

struct Ctx {
    uint32_t h[8];
    uint8_t buf[64];
    uint64_t len = 0;  // bytes hashed so far
    size_t used = 0;   // bytes waiting in buf
};

namespace detail {
inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

inline void block(uint32_t h[8], const uint8_t* p) {
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
    };
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = uint32_t(p[4 * i]) << 24 | uint32_t(p[4 * i + 1]) << 16 | uint32_t(p[4 * i + 2]) << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; ++i) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; ++i) {
        uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
        uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}
}  // namespace detail

inline void init(Ctx& c) {
    static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::memcpy(c.h, iv, sizeof iv);
    c.len = 0;
    c.used = 0;
}

inline void update(Ctx& c, const uint8_t* p, size_t n) {
    c.len += n;
    while (n > 0) {
        size_t take = 64 - c.used < n ? 64 - c.used : n;
        std::memcpy(c.buf + c.used, p, take);
        c.used += take;
        p += take;
        n -= take;
        if (c.used == 64) {
            detail::block(c.h, c.buf);
            c.used = 0;
        }
    }
}

inline void finish(Ctx& c, uint8_t out[32]) {
    const uint64_t bits = c.len * 8;
    const uint8_t one = 0x80, zero = 0;
    update(c, &one, 1);
    while (c.used != 56) update(c, &zero, 1);
    uint8_t lenBytes[8];
    for (int i = 0; i < 8; ++i) lenBytes[i] = uint8_t(bits >> (56 - 8 * i));
    update(c, lenBytes, 8);
    for (int i = 0; i < 8; ++i)
        for (int j = 0; j < 4; ++j) out[4 * i + j] = uint8_t(c.h[i] >> (24 - 8 * j));
}

inline void hash(const uint8_t* p, size_t n, uint8_t out[32]) {
    Ctx c;
    init(c);
    update(c, p, n);
    finish(c, out);
}

}  // namespace sha256

// PBKDF2-HMAC-SHA256 (RFC 8018).
inline void pbkdf2Sha256(const uint8_t* pass, size_t passLen, const uint8_t* salt, size_t saltLen,
                         uint32_t iterations, uint8_t* out, size_t outLen) {
    using namespace sha256;
    uint8_t key[64] = {};
    if (passLen > 64) hash(pass, passLen, key);
    else std::memcpy(key, pass, passLen);
    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; ++i) {
        ipad[i] = key[i] ^ 0x36;
        opad[i] = key[i] ^ 0x5c;
    }
    // The padded key fills exactly one block, so its state is computed once and reused.
    Ctx inner0, outer0;
    init(inner0);
    update(inner0, ipad, 64);
    init(outer0);
    update(outer0, opad, 64);
    auto hmac = [&](const uint8_t* a, size_t an, const uint8_t* b, size_t bn, uint8_t mac[32]) {
        Ctx c = inner0;
        update(c, a, an);
        if (bn) update(c, b, bn);
        uint8_t ih[32];
        finish(c, ih);
        c = outer0;
        update(c, ih, 32);
        finish(c, mac);
    };
    for (uint32_t blockNo = 1; outLen > 0; ++blockNo) {
        const uint8_t be[4] = {uint8_t(blockNo >> 24), uint8_t(blockNo >> 16), uint8_t(blockNo >> 8), uint8_t(blockNo)};
        uint8_t u[32], t[32];
        hmac(salt, saltLen, be, 4, u);
        std::memcpy(t, u, 32);
        for (uint32_t i = 1; i < iterations; ++i) {
            hmac(u, 32, nullptr, 0, u);
            for (int j = 0; j < 32; ++j) t[j] ^= u[j];
        }
        size_t take = outLen < 32 ? outLen : 32;
        std::memcpy(out, t, take);
        out += take;
        outLen -= take;
    }
}

constexpr size_t kMinPassphrase = 12;
constexpr uint32_t kPassphraseIterations = 100000;

// Turns the shared secret into the packet key. Exactly 32 hex characters are used as the
// key itself; anything else is a passphrase: at least kMinPassphrase characters of
// printable ASCII (so the phone keyboard and the PC agree on the bytes) and no ';' or '#'
// (fpv.ini comments). Case-sensitive. Returns nullptr on success, otherwise why not.
inline const char* secretToKey(const char* secret, SipKey& out) {
    const size_t n = std::strlen(secret);
    if (n == 32 && parseSipKey(secret, out)) return nullptr;
    if (n == 0) return "no passphrase set";
    if (n < kMinPassphrase) return "passphrase must be at least 12 characters";
    for (size_t i = 0; i < n; ++i) {
        const char c = secret[i];
        if (c == ';' || c == '#') return "passphrase can't contain ; or #";
        if (c < 0x20 || c > 0x7e) return "passphrase must be plain letters, digits, spaces and punctuation";
    }
    static const char kSalt[] = "fpv-car udp key v1";
    uint8_t k[16];
    pbkdf2Sha256(reinterpret_cast<const uint8_t*>(secret), n, reinterpret_cast<const uint8_t*>(kSalt),
                 sizeof kSalt - 1, kPassphraseIterations, k, sizeof k);
    out.k0 = detail::load64le(k);
    out.k1 = detail::load64le(k + 8);
    return nullptr;
}

}  // namespace fpv
