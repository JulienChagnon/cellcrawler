// Explicit little-endian (de)serialisation so wire formats never depend on struct layout.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace fpv {

class ByteWriter {
public:
    ByteWriter(uint8_t* buf, size_t cap) : buf_(buf), cap_(cap) {}

    void u8(uint8_t v) { put(&v, 1); }
    void u16(uint16_t v) { uint8_t b[2] = {uint8_t(v), uint8_t(v >> 8)}; put(b, 2); }
    void i16(int16_t v) { u16(static_cast<uint16_t>(v)); }
    void u32(uint32_t v) {
        uint8_t b[4] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)};
        put(b, 4);
    }
    void u64(uint64_t v) { u32(uint32_t(v)); u32(uint32_t(v >> 32)); }
    void bytes(const void* p, size_t n) { put(p, n); }

    size_t size() const { return pos_; }
    bool ok() const { return ok_; }

private:
    void put(const void* p, size_t n) {
        if (pos_ + n > cap_) { ok_ = false; return; }
        std::memcpy(buf_ + pos_, p, n);
        pos_ += n;
    }
    uint8_t* buf_;
    size_t cap_, pos_ = 0;
    bool ok_ = true;
};

class ByteReader {
public:
    ByteReader(const uint8_t* buf, size_t len) : buf_(buf), len_(len) {}

    uint8_t u8() { uint8_t b[1] = {0}; get(b, 1); return b[0]; }
    uint16_t u16() { uint8_t b[2] = {0}; get(b, 2); return uint16_t(b[0] | (b[1] << 8)); }
    int16_t i16() { return static_cast<int16_t>(u16()); }
    uint32_t u32() {
        uint8_t b[4] = {0};
        get(b, 4);
        return uint32_t(b[0]) | (uint32_t(b[1]) << 8) | (uint32_t(b[2]) << 16) | (uint32_t(b[3]) << 24);
    }
    uint64_t u64() { uint64_t lo = u32(); uint64_t hi = u32(); return lo | (hi << 32); }

    const uint8_t* rest() const { return buf_ + pos_; }
    size_t remaining() const { return len_ - pos_; }
    bool ok() const { return ok_; }

private:
    void get(uint8_t* p, size_t n) {
        if (pos_ + n > len_) { ok_ = false; return; }
        std::memcpy(p, buf_ + pos_, n);
        pos_ += n;
    }
    const uint8_t* buf_;
    size_t len_, pos_ = 0;
    bool ok_ = true;
};

}  // namespace fpv
