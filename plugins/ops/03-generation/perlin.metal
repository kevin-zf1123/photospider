#include <metal_stdlib>
using namespace metal;

// A work item owns 17 disjoint little-endian base-2^32 integers in managed
// device scratch. No floating-point arithmetic or compiler FP profile enters
// the exact polynomial or its final IEEE bit construction.
constant uint permutation[256] = {
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
struct Coordinate {
  ulong remainder;
  uint lattice, q;
  bool complement;
};
Coordinate decode(ulong raw, bool narrow) {
  const uint mb = narrow ? 23 : 52;
  const uint eb = narrow ? 8 : 11;
  const uint e = uint((raw >> mb) & ((1ul << eb) - 1));
  const bool negative = (raw >> (mb + eb)) != 0;
  ulong significand = raw & ((1ul << mb) - 1);
  if (e)
    significand |= 1ul << mb;
  const int exponent = int(e ? e : 1) - (narrow ? 127 : 1023) - int(mb);
  Coordinate c = {0, 0, 0, false};
  uint integral = 0;
  if (exponent >= 0) {
    if (exponent < 8)
      integral = uint(significand << exponent) & 255;
  } else {
    const uint bits = uint(-exponent);
    integral = bits < 64 ? uint(significand >> bits) & 255 : 0;
    c.remainder = bits < 64 ? significand & ((1ul << bits) - 1) : significand;
    if (c.remainder) {
      const uint zeros = uint(ctz(c.remainder));
      c.remainder >>= zeros;
      c.q = bits - zeros;
      c.complement = negative;
    }
  }
  c.lattice =
      negative ? (0u - integral - uint(c.remainder != 0)) & 255 : integral;
  return c;
}
bool bit(device const uint* a, int index, uint n) {
  return index >= 0 && uint(index) < n * 32 &&
         ((a[index / 32] >> (index % 32)) & 1);
}
ulong rounded(device const uint* a, bool negative, uint q, bool narrow,
              uint n) {
  const uint used = length(a, n);
  if (!used)
    return 0;
  const int high = int((used - 1) * 32 + 31 - clz(a[used - 1]));
  const int mb = narrow ? 23 : 52, bias = narrow ? 127 : 1023;
  int exponent = high - int(16 * q);
  const bool normal = exponent >= 1 - bias;
  const int drop = normal ? high - mb : int(16 * q) + 1 - bias - mb;
  ulong significand = 0;
  for (int i = 0; i <= mb; ++i)
    if (bit(a, drop + i, n))
      significand |= 1ul << i;
  bool sticky = false;
  if (drop > 1) {
    const uint stop = uint(drop - 1);
    for (uint i = 0; i < stop / 32; ++i)
      sticky |= a[i] != 0;
    if (stop % 32)
      sticky |= (a[stop / 32] & ((1u << (stop % 32)) - 1)) != 0;
  }
  if (bit(a, drop - 1, n) && (sticky || (significand & 1)))
    ++significand;
  const ulong sign = negative ? 1ul << (narrow ? 31 : 63) : 0;
  if (!normal)
    return sign | significand;
  if (significand == (1ul << (mb + 1))) {
    significand >>= 1;
    ++exponent;
  }
  return sign | (ulong(exponent + bias) << mb) |
         (significand & ((1ul << mb) - 1));
}
struct Arguments {
  ulong begin;
  uint count, words, input_narrow, output_narrow;
  ulong offset, rank, shape[8], strides[8], origin[8];
};
kernel void perlin_exact(device const uchar* input [[buffer(0)]],
                         device uint* output [[buffer(1)]],
                         device uint* scratch [[buffer(2)]],
                         constant Arguments& args [[buffer(3)]],
                         uint item [[thread_position_in_grid]]) {
  if (item >= args.count)
    return;
  const ulong index = args.begin + item;
  Coordinate coordinates[3];
  uint q = 0;
  ulong linear = index;
  ulong address =
      args.offset - args.origin[args.rank - 1] * args.strides[args.rank - 1];
  for (int axis = int(args.rank) - 2; axis >= 0; --axis) {
    const ulong coordinate = linear % args.shape[axis];
    linear /= args.shape[axis];
    address += (coordinate - args.origin[axis]) * args.strides[axis];
  }
  for (uint axis = 0; axis < 3; ++axis) {
    const ulong at = address + axis * args.strides[args.rank - 1];
    ulong raw = 0;
    for (uint byte = 0; byte < (args.input_narrow ? 4u : 8u); ++byte)
      raw |= ulong(input[at + byte]) << (8 * byte);
    coordinates[axis] = decode(raw, args.input_narrow);
    q = max(q, coordinates[axis].q);
  }
  const uint n = args.words;
  device uint* base = scratch + ulong(item) * 17 * n;
  device uint* d = base + 9 * n;
  device uint* a = base + 10 * n;
  device uint* b = base + 11 * n;
  device uint* c = base + 12 * n;
  device uint* weight = base + 13 * n;
  device uint* dot = base + 14 * n;
  device uint* term = base + 15 * n;
  device uint* numerator = base + 16 * n;
  clear(base, 17 * n);
  bool negative = false;
  if (q) {
    power(d, q, n);
    for (uint axis = 0; axis < 3; ++axis) {
      device uint* fraction = base + axis * n;
      device uint* fade0 = base + (3 + axis * 2) * n;
      device uint* fade1 = fade0 + n;
      clear(a, n);
      a[0] = uint(coordinates[axis].remainder);
      a[1] = uint(coordinates[axis].remainder >> 32);
      shift(a, q - coordinates[axis].q, fraction, n);
      if (coordinates[axis].complement) {
        copy(a, d, n);
        subtract(a, fraction, n);
        copy(fraction, a, n);
      }
      multiply(fraction, fraction, a, n);
      multiply(a, fraction, b, n);
      times(a, 6, n);
      power(c, 2 * q, n);
      times(c, 10, n);
      add(a, c, n);
      shift(fraction, q, c, n);
      times(c, 15, n);
      subtract(a, c, n);
      multiply(a, b, fade1, n);
      power(fade0, 5 * q, n);
      subtract(fade0, fade1, n);
    }
    for (uint corner = 0; corner < 8; ++corner) {
      const uint x = corner & 1, y = (corner >> 1) & 1, z = corner >> 2;
      const uint hash =
          permutation
              [(permutation[(permutation[(coordinates[0].lattice + x) & 255] +
                             coordinates[1].lattice + y) &
                            255] +
                coordinates[2].lattice + z) &
               255] &
          15;
      const uint u = hash < 8 ? 0 : 1;
      const uint v = hash < 4 ? 1 : (hash == 12 || hash == 14) ? 0 : 2;
      const bool upper_u = (corner >> u) & 1, upper_v = (corner >> v) & 1;
      copy(dot, upper_u ? d : base + u * n, n);
      if (upper_u)
        subtract(dot, base + u * n, n);
      copy(a, upper_v ? d : base + v * n, n);
      if (upper_v)
        subtract(a, base + v * n, n);
      bool sign = upper_u != bool(hash & 1);
      const bool other_sign = upper_v != bool(hash & 2);
      if (sign == other_sign)
        add(dot, a, n);
      else if (compare(dot, a, n) >= 0)
        subtract(dot, a, n);
      else {
        subtract(a, dot, n);
        copy(dot, a, n);
        sign = other_sign;
      }
      multiply(base + (3 + x) * n, base + (5 + y) * n, a, n);
      multiply(a, base + (7 + z) * n, weight, n);
      multiply(weight, dot, term, n);
      if (negative == sign)
        add(numerator, term, n);
      else if (compare(numerator, term, n) >= 0)
        subtract(numerator, term, n);
      else {
        subtract(term, numerator, n);
        copy(numerator, term, n);
        negative = sign;
      }
    }
  }
  const ulong result = rounded(numerator, negative, q, args.output_narrow, n);
  const ulong at = index * (args.output_narrow ? 1 : 2);
  output[at] = uint(result);
  if (!args.output_narrow)
    output[at + 1] = uint(result >> 32);
}
