#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace ps::content_internal {
/** @brief Streaming SHA-256 for disposable content identities, not signatures.
 * @note Instances are local to a caller; no shared mutable state or allocation.
 */
class Sha256 final {
 public:
  void bytes(const void* pointer, std::size_t size) noexcept {
    const auto* p = static_cast<const std::uint8_t*>(pointer);
    length_ += static_cast<std::uint64_t>(size);
    while (size != 0) {
      const auto n = size < 64 - used_ ? size : 64 - used_;
      std::memcpy(buffer_.data() + used_, p, n);
      p += n;
      size -= n;
      used_ += n;
      if (used_ == 64) {
        block();
        used_ = 0;
      }
    }
  }
  void integer(std::uint64_t value) noexcept {
    std::uint8_t data[8];
    for (unsigned i = 0; i < 8; ++i)
      data[i] = static_cast<std::uint8_t>(value >> (8 * i));
    bytes(data, 8);
  }
  void text(const std::string& value) noexcept {
    integer(value.size());
    bytes(value.data(), value.size());
  }
  std::string finish() const {
    auto copy = *this;
    const auto bits = length_ * 8;
    const std::uint8_t one = 0x80, zero = 0;
    copy.bytes(&one, 1);
    while (copy.used_ != 56)
      copy.bytes(&zero, 1);
    std::uint8_t tail[8];
    for (unsigned i = 0; i < 8; ++i)
      tail[7 - i] = static_cast<std::uint8_t>(bits >> (8 * i));
    copy.bytes(tail, 8);
    static constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (auto word : copy.state_)
      for (int shift = 28; shift >= 0; shift -= 4)
        result.push_back(hex[(word >> shift) & 15]);
    return result;
  }

 private:
  static std::uint32_t rotate(std::uint32_t v, unsigned n) noexcept {
    return (v >> n) | (v << (32 - n));
  }
  void block() noexcept {
    static constexpr std::uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
        0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
        0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
        0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
        0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
        0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
        0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    std::uint32_t w[64];
    for (unsigned i = 0; i < 16; ++i) {
      w[i] = 0;
      for (unsigned j = 0; j < 4; ++j)
        w[i] = (w[i] << 8) | buffer_[4 * i + j];
    }
    for (unsigned i = 16; i < 64; ++i) {
      const auto a = w[i - 15], b = w[i - 2];
      w[i] = w[i - 16] + (rotate(a, 7) ^ rotate(a, 18) ^ (a >> 3)) + w[i - 7] +
             (rotate(b, 17) ^ rotate(b, 19) ^ (b >> 10));
    }
    auto a = state_[0], b = state_[1], c = state_[2], d = state_[3],
         e = state_[4], f = state_[5], g = state_[6], h = state_[7];
    for (unsigned i = 0; i < 64; ++i) {
      const auto t1 = h + (rotate(e, 6) ^ rotate(e, 11) ^ rotate(e, 25)) +
                      ((e & f) ^ (~e & g)) + k[i] + w[i];
      const auto t2 = (rotate(a, 2) ^ rotate(a, 13) ^ rotate(a, 22)) +
                      ((a & b) ^ (a & c) ^ (b & c));
      h = g;
      g = f;
      f = e;
      e = d + t1;
      d = c;
      c = b;
      b = a;
      a = t1 + t2;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
  }
  std::array<std::uint32_t, 8> state_{{0x6a09e667, 0xbb67ae85, 0x3c6ef372,
                                       0xa54ff53a, 0x510e527f, 0x9b05688c,
                                       0x1f83d9ab, 0x5be0cd19}};
  std::array<std::uint8_t, 64> buffer_{};
  std::size_t used_ = 0;
  std::uint64_t length_ = 0;
};
}  // namespace ps::content_internal
