[[vk::binding(0, 0)]] StructuredBuffer<uint8_t> input;
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> output;
[[vk::binding(2, 0)]] StructuredBuffer<uint64_t> kx;
[[vk::binding(3, 0)]] StructuredBuffer<uint64_t> ky;
[[vk::binding(4, 0)]] RWStructuredBuffer<uint> scratch;
#include "../gpu/integer.slang"

// N and D are integers in weight/sample units of 2^-1074. The result is
// RN(N/D * 2^-1074). At most UINT64_MAX taps imply N<2^4310, D<2^2212;
// 136 uint32 words also cover the aligned divisor and doubled remainder.
static const uint words = 136;
static const uint lane_words = 8 + 10 * words;
// IEEE values in the common 2^-1074 quantum have many low zero words.
// Trim those exact powers of two before schoolbook multiplication. The full
// destination is still cleared, and retained rows preserve carry order.
void multiply_trimmed(uint64_t a, uint64_t b, uint64_t out) {
  const uint na = length(a, words), nb = length(b, words);
  uint first_a = 0, first_b = 0;
  while (first_a < na && (scratch[a + (first_a)] == 0))
    ++first_a;
  while (first_b < nb && (scratch[b + (first_b)] == 0))
    ++first_b;
  clear(out, words);
  for (uint i = first_a; i < na; ++i) {
    if ((scratch[a + (i)] == 0))
      continue;
    uint64_t carry = 0;
    for (uint j = first_b; j < nb && i + j < words; ++j) {
      const uint64_t v = uint64_t(scratch[a + (i)]) * scratch[b + (j)] +
                         scratch[out + (i + j)] + carry;
      scratch[out + (i + j)] = uint(v);
      carry = v >> 32;
    }
    if (i + nb < words)
      scratch[out + (i + nb)] = uint(carry);
  }
}
struct Arguments {
  uint64_t begin, tap_begin, tap_end, offset, nx, ny, cval;
  uint64_t x_offset, y_offset;
  uint64_t shape[8], strides[8], origin[8];
  uint count, rank, x, y, boundary, narrow, identity, initialize, finish;
};
uint64_t load(uint64_t address, bool narrow) {
  uint64_t result = 0;
  for (uint i = 0; i < (narrow ? 4u : 8u); ++i)
    result |= uint64_t(input[address + i]) << (8 * i);
  return result;
}
void set_value(uint64_t out, uint64_t raw, bool narrow) {
  clear(out, words);
  const uint mb = narrow ? 23 : 52;
  const uint exponent = uint(raw >> mb) & (narrow ? 255u : 2047u);
  uint64_t significand = raw & ((uint64_t(1) << mb) - 1);
  if (exponent != 0)
    significand |= uint64_t(1) << mb;
  const uint shift_bits = uint(int(exponent != 0 ? exponent : 1) -
                               (narrow ? 127 : 1023) - int(mb) + 1074);
  const uint at = shift_bits / 32, rest = shift_bits % 32;
  for (uint i = 0; i < 2; ++i) {
    const uint part = uint(significand >> (32 * i));
    scratch[out + (at + i)] |= part << rest;
    if (rest != 0)
      scratch[out + (at + i + 1)] |= part >> (32 - rest);
  }
}
bool mapped(int64_t index, uint64_t extent, uint boundary,
            out uint64_t output) {
  output = 0;
  if (index >= 0 && uint64_t(index) < extent) {
    output = uint64_t(index);
    return true;
  }
  if (boundary == 0)
    return false;
  if (boundary == 1) {
    output = index < 0 ? 0 : extent - 1;
    return true;
  }
  if (extent == 1) {
    output = 0;
    return true;
  }
  const uint64_t period = boundary == 2   ? extent
                          : boundary == 3 ? 2 * extent
                                          : 2 * extent - 2;
  // Vulkan without maintenance8 defines signed remainder only for
  // nonnegative operands. Form the negative-index remainder with magnitudes.
  uint64_t t = (index < 0 ? uint64_t(-index) : uint64_t(index)) % period;
  if (index < 0 && t != 0)
    t = period - t;
  output = t < extent ? t : period - t - (boundary == 3 ? 1 : 0);
  return true;
}
int top(uint64_t a) {
  const uint count = length(a, words);
  return count != 0
             ? int((count - 1) * 32 + firstbithigh(scratch[a + (count - 1)]))
             : -1;
}
uint64_t rounded_ratio(uint64_t numerator, uint64_t denominator, bool negative,
                       bool narrow, uint64_t temporary, uint64_t divisor,
                       uint64_t remainder) {
  // The center coefficient is exactly one on each axis, so D is positive.
  const int ntop = top(numerator), dtop = top(denominator);
  if (ntop < 0)
    return 0;
  int k = ntop - dtop;
  if (k >= 0) {
    shift(denominator, uint(k), temporary, words);
    if (compare(numerator, temporary, words) < 0)
      --k;
  } else {
    shift(numerator, uint(-k), temporary, words);
    if (compare(temporary, denominator, words) < 0)
      --k;
  }
  const int mb = narrow ? 23 : 52, bias = narrow ? 127 : 1023;
  int exponent = k - 1074;
  const uint64_t sign = negative ? uint64_t(1) << (narrow ? 31 : 63) : 0;
  const uint64_t inf =
      narrow ? uint64_t(0x7f800000) : uint64_t(0x7ff0000000000000);
  if (exponent > bias)
    return sign | inf;
  const bool normal = exponent >= 1 - bias;
  // The input quantum is the binary64 subnormal unit, so only the divisor
  // needs a left shift, even for a subnormal output (0 or 925 bits).
  const uint drop = uint(max(k - mb, narrow ? 925 : 0));
  shift(denominator, drop, divisor, words);
  copy(remainder, numerator, words);
  uint64_t significand = 0;
  for (int bit = mb; bit >= 0; --bit) {
    shift(divisor, uint(bit), temporary, words);
    if (compare(remainder, temporary, words) >= 0) {
      subtract(remainder, temporary, words);
      significand |= uint64_t(1) << bit;
    }
  }
  shift(remainder, 1, temporary, words);
  const int halfway = compare(temporary, divisor, words);
  if (halfway > 0 || (halfway == 0 && (significand & 1) != 0))
    ++significand;
  if (!normal)
    return sign | significand;
  if (significand == (uint64_t(1) << (mb + 1))) {
    significand >>= 1;
    ++exponent;
  }
  if (exponent > bias)
    return sign | inf;
  return sign | (uint64_t(exponent + bias) << mb) |
         (significand & ((uint64_t(1) << mb) - 1));
}
[[vk::binding(5, 0)]] ConstantBuffer<Arguments> arguments;
[shader("compute")][numthreads(64, 1, 1)] void gaussian_exact(
    uint3 dispatch_id : SV_DispatchThreadID) {
  const uint item = dispatch_id.x;
  const Arguments args = arguments;
  if (item >= args.count)
    return;
  uint64_t state = uint64_t(item) * lane_words;
  uint64_t numerator = state + 8;
  uint64_t denominator = numerator + words;
  uint64_t x = denominator + words;
  uint64_t y = x + words;
  uint64_t product = y + words;
  uint64_t value = product + words;
  uint64_t term = value + words;
  uint64_t temporary = term + words;
  uint64_t divisor = temporary + words;
  uint64_t remainder = divisor + words;
  if (args.initialize != 0) {
    clear(state, lane_words);
    scratch[state + (2)] = 1;
  }
  const uint64_t index = args.begin + item;
  uint64_t coordinates[8];
  uint64_t linear = index;
  for (int axis = int(args.rank) - 1; axis >= 0; --axis) {
    coordinates[axis] = linear % args.shape[axis];
    linear /= args.shape[axis];
  }
  const uint64_t sign = uint64_t(1) << ((args.narrow != 0) ? 31 : 63);
  const uint64_t inf =
      (args.narrow != 0) ? uint64_t(0x7f800000) : uint64_t(0x7ff0000000000000);
  const uint64_t quiet = uint64_t(1) << ((args.narrow != 0) ? 22 : 51);
  for (uint64_t tap = args.tap_begin; tap < args.tap_end; ++tap) {
    const uint64_t j = tap / args.nx, i = tap % args.nx;
    const uint64_t wx = kx[args.x_offset + i], wy = ky[args.y_offset + j];
    if (wx == 0 || wy == 0)
      continue;
    uint64_t xx = 0, yy = 0;
    // Static table-byte admission bounds the radii below 2^59; shape <=2^40.
    // These signed boundary expressions therefore fit int64_t without int128.
    const bool inside_y = mapped(
        int64_t(coordinates[args.y]) + int64_t((args.ny - 1) / 2) - int64_t(j),
        args.shape[args.y], args.boundary, yy);
    const bool inside_x = mapped(
        int64_t(coordinates[args.x]) + int64_t((args.nx - 1) / 2) - int64_t(i),
        args.shape[args.x], args.boundary, xx);
    const bool inside = inside_y && inside_x;
    uint64_t raw = args.cval;
    const bool narrow = inside && args.narrow != 0;
    if (inside) {
      uint64_t address = args.offset;
      for (uint axis = 0; axis < args.rank; ++axis) {
        const uint64_t at = axis == args.x   ? xx
                            : axis == args.y ? yy
                                             : coordinates[axis];
        address += (at - args.origin[axis]) * args.strides[axis];
      }
      raw = load(address, narrow);
    }
    if (args.identity != 0) {
      scratch[state + (5)] = uint(raw);
      scratch[state + (6)] = uint(raw >> 32);
      continue;
    }
    set_value(x, wx, false);
    set_value(y, wy, false);
    multiply_trimmed(x, y, product);
    add(denominator, product, words);
    const uint mb = narrow ? 23 : 52;
    const uint64_t exponent_mask =
        narrow ? uint64_t(0x7f800000) : uint64_t(0x7ff0000000000000);
    const bool negative = (raw >> (narrow ? 31 : 63)) != 0;
    const uint64_t magnitude = raw & ~(uint64_t(1) << (narrow ? 31 : 63));
    const bool special = (raw & exponent_mask) == exponent_mask;
    const bool nan = special && (raw & ((uint64_t(1) << mb) - 1)) != 0;
    if (nan && (scratch[state + (3)] == 0) && (scratch[state + (4)] == 0)) {
      const uint64_t converted = raw | quiet;
      scratch[state + (3)] = uint(converted);
      scratch[state + (4)] = uint(converted >> 32);
    }
    if (special && !nan)
      scratch[state + (1)] |= negative ? 2 : 1;
    scratch[state + (2)] =
        scratch[state + (2)] != 0 && negative && magnitude == 0;
    if (special || (scratch[state + (1)] != 0) || (scratch[state + (3)] != 0) ||
        (scratch[state + (4)] != 0))
      continue;
    set_value(value, raw, narrow);
    multiply_trimmed(product, value, term);
    if (bool(scratch[state + (0)]) == negative)
      add(numerator, term, words);
    else if (compare(numerator, term, words) >= 0)
      subtract(numerator, term, words);
    else {
      subtract(term, numerator, words);
      copy(numerator, term, words);
      scratch[state + (0)] = negative;
    }
  }
  if (args.finish == 0)
    return;
  uint64_t result;
  if (args.identity != 0)
    result =
        uint64_t(scratch[state + (5)]) | (uint64_t(scratch[state + (6)]) << 32);
  else if ((scratch[state + (3)] != 0) || (scratch[state + (4)] != 0))
    result =
        uint64_t(scratch[state + (3)]) | (uint64_t(scratch[state + (4)]) << 32);
  else if ((scratch[state + (1)] != 0))
    result = scratch[state + (1)] == 3
                 ? inf | quiet
                 : inf | (scratch[state + (1)] == 2 ? sign : 0);
  else if (scratch[state + (2)] != 0)
    result = sign;
  else
    result = rounded_ratio(numerator, denominator, scratch[state + (0)] != 0,
                           (args.narrow != 0), temporary, divisor, remainder);
  const uint64_t at = index * ((args.narrow != 0) ? 1 : 2);
  output[at] = uint(result);
  if (args.narrow == 0)
    output[at + 1] = uint(result >> 32);
}
