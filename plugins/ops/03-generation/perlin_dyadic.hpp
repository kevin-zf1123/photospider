#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

#include "03-generation/runtime.hpp"

namespace ps::plugin_internal::generation {
// CPU form of the same integer polynomial used by perlin_metal.hpp.
// Active limbs avoid gcd reduction and whole-capacity clearing per operation.
// Words=80 covers d<=149; Words=544 covers every finite binary64 coordinate.
template <unsigned Words>
class PerlinDyadic final {
 public:
  using uint = std::uint32_t;
  using ulong = std::uint64_t;
  const Work& work;
  std::array<uint, 17 * (Words + 1)> storage;
  explicit PerlinDyadic(const Work& consumer) : work(consumer) {}
  struct Coordinate {
    uint cell, denominator_bits;
    ulong remainder;
    uint negative_fraction;
  };
  inline uint* slot(uint* p, uint i) { return p + ulong(i) * (Words + 1); }
  inline uint limb(const uint* p, uint i) { return i < p[0] ? p[i + 1] : 0; }
  inline void zero(uint* p) { p[0] = 0; }
  inline void trim(uint* p, uint n) {
    while (n && !p[n]) {
      --n;
    }
    p[0] = n;
  }
  inline void copy(uint* out, const uint* a) {
    if (out == a) {
      return;
    }
    uint n = a[0];
    for (uint i = 0; i < n; ++i) {
      out[i + 1] = a[i + 1];
    }
    out[0] = n;
  }
  inline void u64(uint* out, ulong n) {
    if (!n) {
      zero(out);
      return;
    }
    out[1] = uint(n);
    out[2] = uint(n >> 32);
    out[0] = out[2] ? 2 : 1;
  }
  inline void pow2(uint* out, uint e) {
    uint n = e >> 5;
    work.poll(n + 1);
    for (uint i = 0; i < n; ++i) {
      out[i + 1] = 0;
    }
    out[n + 1] = 1u << (e & 31);
    out[0] = n + 1;
  }
  inline int compare(const uint* a, const uint* b) {
    if (a[0] != b[0]) {
      return a[0] < b[0] ? -1 : 1;
    }
    for (int i = static_cast<int>(a[0]); i > 0; --i) {
      if (a[i] != b[i]) {
        return a[i] < b[i] ? -1 : 1;
      }
    }
    return 0;
  }
  inline void add(uint* out, const uint* a, const uint* b) {
    uint na = a[0], nb = b[0], n = std::max(na, nb);
    work.poll(n + 1);
    ulong carry = 0;
    for (uint i = 0; i < n; ++i) {
      ulong v = (i < na ? ulong(a[i + 1]) : 0) +
                (i < nb ? ulong(b[i + 1]) : 0) + carry;
      out[i + 1] = uint(v);
      carry = v >> 32;
    }
    if (carry) {
      out[++n] = uint(carry);
    }
    out[0] = n;
  }
  inline void sub(uint* out, const uint* a, const uint* b) {
    uint na = a[0], nb = b[0];
    work.poll(na + 1);
    ulong borrow = 0;
    for (uint i = 0; i < na; ++i) {
      ulong av = a[i + 1], bv = (i < nb ? ulong(b[i + 1]) : 0) + borrow;
      out[i + 1] = uint(av - bv);
      borrow = av < bv ? 1 : 0;
    }
    trim(out, na);
  }
  inline void mul_small(uint* out, const uint* a, uint factor) {
    uint n = a[0];
    work.poll(n + 1);
    if (!n || !factor) {
      zero(out);
      return;
    }
    ulong carry = 0;
    for (uint i = 0; i < n; ++i) {
      ulong v = ulong(a[i + 1]) * factor + carry;
      out[i + 1] = uint(v);
      carry = v >> 32;
    }
    if (carry) {
      out[++n] = uint(carry);
    }
    out[0] = n;
  }
  // Output never aliases either input. At most 16*d+O(1) bits are required.
  inline void mul(uint* out, const uint* a, const uint* b) {
    uint na = a[0], nb = b[0];
    if (!na || !nb) {
      zero(out);
      return;
    }
    uint n = na + nb;
    for (uint i = 0; i < n; ++i) {
      out[i + 1] = 0;
    }
    for (uint i = 0; i < na; ++i) {
      work.poll(nb + 1);
      ulong carry = 0, av = a[i + 1];
      for (uint j = 0; j < nb; ++j) {
        ulong v = av * ulong(b[j + 1]) + ulong(out[i + j + 1]) + carry;
        out[i + j + 1] = uint(v);
        carry = v >> 32;
      }
      out[i + nb + 1] = uint(carry);
    }
    trim(out, n);
  }
  inline void shl(uint* out, const uint* a, uint shift) {
    uint na = a[0];
    if (!na) {
      zero(out);
      return;
    }
    if (!shift) {
      copy(out, a);
      return;
    }
    uint ws = shift >> 5, bs = shift & 31, n = na + ws + (bs ? 1 : 0);
    work.poll(n + 1);
    for (int k = static_cast<int>(n) - 1; k >= 0; --k) {
      int source = k - static_cast<int>(ws);
      uint v = 0;
      if (source >= 0 && source < static_cast<int>(na)) {
        v = bs ? a[uint(source) + 1] << bs : a[uint(source) + 1];
      }
      if (bs && source > 0 && source - 1 < static_cast<int>(na)) {
        v |= a[uint(source)] >> (32 - bs);
      }
      out[uint(k) + 1] = v;
    }
    trim(out, n);
  }
  inline int signed_add(uint* out, const uint* a, int sa, const uint* b,
                        int sb) {
    if (!a[0]) {
      copy(out, b);
      return out[0] ? sb : 0;
    }
    if (!b[0]) {
      copy(out, a);
      return sa;
    }
    if (sa == sb) {
      add(out, a, b);
      return sa;
    }
    int c = compare(a, b);
    if (!c) {
      zero(out);
      return 0;
    }
    if (c > 0) {
      sub(out, a, b);
      return sa;
    }
    sub(out, b, a);
    return sb;
  }
  inline uint bit_length(const uint* p) {
    return p[0] ? (p[0] - 1) * 32 + 32 - __builtin_clz(p[p[0]]) : 0;
  }
  inline uint bit(const uint* p, uint b) {
    return (b >> 5) < p[0] ? (p[(b >> 5) + 1] >> (b & 31)) & 1 : 0;
  }
  inline bool any_low(const uint* p, uint bits) {
    uint n = bits >> 5;
    for (uint i = 0; i < std::min(n, p[0]); ++i) {
      if (p[i + 1]) {
        return true;
      }
    }
    uint tail = bits & 31;
    return tail && n < p[0] && (p[n + 1] & ((1u << tail) - 1));
  }
  inline ulong extract(const uint* p, uint shift) {
    uint w = shift >> 5, b = shift & 31;
    ulong lo = ulong(limb(p, w)) | (ulong(limb(p, w + 1)) << 32);
    return b ? (lo >> b) | (ulong(limb(p, w + 2)) << (64 - b)) : lo;
  }
  inline ulong rounded(const uint* p, uint denominator, int sign, uint small) {
    uint length = bit_length(p);
    if (!length) {
      return 0;
    }
    uint f = small ? 23 : 52, precision = f + 1;
    int minimum = small ? -126 : -1022, maximum = small ? 127 : 1023,
        bias = small ? 127 : 1023;
    ulong s = sign < 0 ? ulong(1) << (small ? 31 : 63) : 0;
    int e = static_cast<int>(length) - 1 - static_cast<int>(denominator),
        q = std::max(e - static_cast<int>(f), minimum - static_cast<int>(f)),
        shift = static_cast<int>(denominator) + q;
    ulong sig;
    if (shift <= 0) {
      sig = extract(p, 0) << uint(-shift);
    } else {
      sig = extract(p, uint(shift));
      if (bit(p, uint(shift - 1)) &&
          (any_low(p, uint(shift - 1)) || (sig & 1))) {
        ++sig;
      }
    }
    if (!sig) {
      return s;
    }
    ulong hidden = ulong(1) << f;
    if (e < minimum) {
      return s | (sig >= hidden ? hidden : sig);
    }
    if (sig == (ulong(1) << precision)) {
      sig >>= 1;
      ++e;
    }
    if (e > maximum) {
      return s | ((ulong(small ? 255 : 2047)) << f);
    }
    return s | (ulong(e + bias) << f) | (sig - hidden);
  }
  inline Coordinate decode(ulong raw, uint small) {
    Coordinate r = {0, 0, 0, 0};
    ulong sig;
    uint neg;
    int exponent;
    if (small) {
      uint b = uint(raw), e = (b >> 23) & 255;
      neg = b >> 31;
      sig = ulong((b & 0x7fffff) | (e ? 0x800000 : 0));
      exponent = e ? static_cast<int>(e) - 150 : -149;
    } else {
      uint e = uint((raw >> 52) & 2047);
      neg = uint(raw >> 63);
      sig = (raw & ((ulong(1) << 52) - 1)) | (e ? ulong(1) << 52 : 0);
      exponent = e ? static_cast<int>(e) - 1075 : -1074;
    }
    if (!sig) {
      return r;
    }
    if (exponent >= 0) {
      uint v = exponent < 8 ? (uint(sig & 255) << uint(exponent)) & 255 : 0;
      r.cell = neg ? (0u - v) & 255 : v;
      return r;
    }
    uint d = uint(-exponent);
    ulong ip = d >= 64 ? 0 : sig >> d,
          rem = d >= 64 ? sig : sig & ((ulong(1) << d) - 1);
    uint v = uint(ip & 255);
    r.cell = neg ? (0u - v - (rem ? 1u : 0u)) & 255 : v;
    if (!rem) {
      return r;
    }
    while (!(rem & 1)) {
      rem >>= 1;
      --d;
    }
    r.denominator_bits = d;
    r.remainder = rem;
    r.negative_fraction = neg;
    return r;
  }
  inline void fade(const uint* f, uint d, uint* out, uint* complement, uint* t0,
                   uint* t1, uint* t2) {
    if (!f[0]) {
      zero(out);
      pow2(complement, 0);
      return;
    }
    mul(t0, f, f);
    mul(t1, t0, f);
    mul_small(t0, f, 6);
    pow2(t2, d);
    mul_small(t2, t2, 15);
    sub(t2, t2, t0);
    mul(t0, t2, f);
    pow2(t2, 2 * d);
    mul_small(t2, t2, 10);
    sub(t2, t2, t0);
    mul(out, t1, t2);
    pow2(complement, 5 * d);
    sub(complement, complement, out);
  }
  inline void prepare_axis(uint* p, uint axis, Coordinate c, uint common) {
    uint* f = slot(p, 6 + 2 * axis);
    uint* co = slot(p, 7 + 2 * axis);
    uint* t0 = slot(p, 12);
    uint* t1 = slot(p, 13);
    uint* t2 = slot(p, 14);
    if (c.negative_fraction) {
      pow2(f, c.denominator_bits);
      u64(t0, c.remainder);
      sub(f, f, t0);
    } else {
      u64(f, c.remainder);
    }
    fade(f, c.denominator_bits, slot(p, 2 * axis), slot(p, 2 * axis + 1), t0,
         t1, t2);
    shl(f, f, common - c.denominator_bits);
    pow2(co, common);
    sub(co, co, f);
  }
  inline static constexpr uint permutation[256] = {
      151, 160, 137, 91,  90,  15,  131, 13,  201, 95,  96,  53,  194, 233, 7,
      225, 140, 36,  103, 30,  69,  142, 8,   99,  37,  240, 21,  10,  23,  190,
      6,   148, 247, 120, 234, 75,  0,   26,  197, 62,  94,  252, 219, 203, 117,
      35,  11,  32,  57,  177, 33,  88,  237, 149, 56,  87,  174, 20,  125, 136,
      171, 168, 68,  175, 74,  165, 71,  134, 139, 48,  27,  166, 77,  146, 158,
      231, 83,  111, 229, 122, 60,  211, 133, 230, 220, 105, 92,  41,  55,  46,
      245, 40,  244, 102, 143, 54,  65,  25,  63,  161, 1,   216, 80,  73,  209,
      76,  132, 187, 208, 89,  18,  169, 200, 196, 135, 130, 116, 188, 159, 86,
      164, 100, 109, 198, 173, 186, 3,   64,  52,  217, 226, 250, 124, 123, 5,
      202, 38,  147, 118, 126, 255, 82,  85,  212, 207, 206, 59,  227, 47,  16,
      58,  17,  182, 189, 28,  42,  223, 183, 170, 213, 119, 248, 152, 2,   44,
      154, 163, 70,  221, 153, 101, 155, 167, 43,  172, 9,   129, 22,  39,  253,
      19,  98,  108, 110, 79,  113, 224, 232, 178, 185, 112, 104, 218, 246, 97,
      228, 251, 34,  242, 193, 238, 210, 144, 12,  191, 179, 162, 241, 81,  51,
      145, 235, 249, 14,  239, 107, 49,  192, 214, 31,  181, 199, 106, 157, 184,
      84,  204, 176, 115, 121, 50,  45,  127, 4,   150, 254, 138, 236, 205, 93,
      222, 114, 67,  29,  24,  72,  243, 141, 128, 195, 78,  66,  215, 61,  156,
      180};
  inline uint hash(uint x, uint y, uint z, uint corner) {
    uint a = permutation[(x + (corner & 1)) & 255],
         b = permutation[(a + y + ((corner >> 1) & 1)) & 255];
    return permutation[(b + z + (corner >> 2)) & 255];
  }
  inline int gradient(uint* p, uint corner, uint h, uint* out) {
    h &= 15;
    uint ua = h < 8 ? 0 : 1, va = h < 4 ? 1 : (h == 12 || h == 14) ? 0 : 2;
    uint uc = (corner >> ua) & 1, vc = (corner >> va) & 1;
    bool un = ((h & 1) != 0) != (uc != 0), vn = ((h & 2) != 0) != (vc != 0);
    return signed_add(out, slot(p, 6 + 2 * ua + uc), un ? -1 : 1,
                      slot(p, 6 + 2 * va + vc), vn ? -1 : 1);
  }

  std::uint64_t evaluate(const std::array<std::uint64_t, 3>& input,
                         bool input_small, bool output_small) {
    auto* p = storage.data();
    const auto x = decode(input[0], input_small),
               y = decode(input[1], input_small),
               z = decode(input[2], input_small);
    const auto common = std::max(
        x.denominator_bits, std::max(y.denominator_bits, z.denominator_bits));
    if (!common) {
      return 0;
    }
    prepare_axis(p, 0, x, common);
    prepare_axis(p, 1, y, common);
    prepare_axis(p, 2, z, common);
    auto* xy = slot(p, 12);
    auto* xyz = slot(p, 13);
    auto* g = slot(p, 14);
    auto* term = slot(p, 15);
    auto* sum = slot(p, 16);
    zero(sum);
    int sign = 0;
    for (uint c = 0; c < 8; ++c) {
      work.poll();
      const auto* wx = slot(p, (c & 1) ? 0 : 1);
      const auto* wy = slot(p, (c & 2) ? 2 : 3);
      const auto* wz = slot(p, (c & 4) ? 4 : 5);
      if (!wx[0] || !wy[0] || !wz[0]) {
        continue;
      }
      mul(xy, wx, wy);
      mul(xyz, xy, wz);
      const int gs = gradient(p, c, hash(x.cell, y.cell, z.cell, c), g);
      if (!gs) {
        continue;
      }
      mul(term, xyz, g);
      sign = signed_add(sum, sum, sign, term, gs);
    }
    return rounded(
        sum,
        5 * (x.denominator_bits + y.denominator_bits + z.denominator_bits) +
            common,
        sign, output_small);
  }
};
struct PerlinScratch final {
  PerlinDyadic<80> compact;
  PerlinDyadic<544> wide;
  explicit PerlinScratch(const Work& work) : compact(work), wide(work) {}
};
}  // namespace ps::plugin_internal::generation
