#include <metal_stdlib>
using namespace metal;

// N and D are integers in weight/sample units of 2^-1074. The result is
// RN(N/D * 2^-1074). At most UINT64_MAX taps imply N<2^4310, D<2^2212;
// 136 uint32 words also cover the aligned divisor and doubled remainder.
constant uint words = 136;
constant uint lane_words = 8 + 10 * words;
// IEEE values in the common 2^-1074 quantum have many low zero words.
// Trim those exact powers of two before schoolbook multiplication. The full
// destination is still cleared, and retained rows preserve carry order.
void multiply_trimmed(device const uint* a, device const uint* b,
                      device uint* out) {
  const uint na = length(a, words), nb = length(b, words);
  uint first_a = 0, first_b = 0;
  while (first_a < na && !a[first_a])
    ++first_a;
  while (first_b < nb && !b[first_b])
    ++first_b;
  clear(out, words);
  for (uint i = first_a; i < na; ++i) {
    if (!a[i])
      continue;
    ulong carry = 0;
    for (uint j = first_b; j < nb && i + j < words; ++j) {
      const ulong v = ulong(a[i]) * b[j] + out[i + j] + carry;
      out[i + j] = uint(v);
      carry = v >> 32;
    }
    if (i + nb < words)
      out[i + nb] = uint(carry);
  }
}
struct Arguments {
  ulong begin, tap_begin, tap_end, offset, nx, ny, cval;
  ulong shape[8], strides[8], origin[8];
  uint count, rank, x, y, boundary, narrow, identity, initialize, finish;
};
ulong load(device const uchar* source, ulong address, bool narrow) {
  ulong result = 0;
  for (uint i = 0; i < (narrow ? 4u : 8u); ++i)
    result |= ulong(source[address + i]) << (8 * i);
  return result;
}
void set_value(device uint* out, ulong raw, bool narrow) {
  clear(out, words);
  const uint mb = narrow ? 23 : 52;
  const uint exponent = uint(raw >> mb) & (narrow ? 255u : 2047u);
  ulong significand = raw & ((1ul << mb) - 1);
  if (exponent)
    significand |= 1ul << mb;
  const uint shift_bits = uint(int(exponent ? exponent : 1) -
                               (narrow ? 127 : 1023) - int(mb) + 1074);
  const uint at = shift_bits / 32, rest = shift_bits % 32;
  for (uint i = 0; i < 2; ++i) {
    const uint part = uint(significand >> (32 * i));
    out[at + i] |= part << rest;
    if (rest)
      out[at + i + 1] |= part >> (32 - rest);
  }
}
bool mapped(long index, ulong extent, uint boundary, thread ulong& output) {
  if (index >= 0 && ulong(index) < extent) {
    output = ulong(index);
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
  const long period = long(boundary == 2   ? extent
                           : boundary == 3 ? 2 * extent
                                           : 2 * extent - 2);
  long t = index % period;
  if (t < 0)
    t += period;
  output = ulong(t) < extent ? ulong(t)
                             : ulong(period - t - (boundary == 3 ? 1 : 0));
  return true;
}
int top(device const uint* a) {
  const uint count = length(a, words);
  return count ? int((count - 1) * 32 + 31 - clz(a[count - 1])) : -1;
}
ulong rounded_ratio(device const uint* numerator,
                    device const uint* denominator, bool negative, bool narrow,
                    device uint* temporary, device uint* divisor,
                    device uint* remainder) {
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
  const ulong sign = negative ? 1ul << (narrow ? 31 : 63) : 0;
  const ulong inf = narrow ? 0x7f800000ul : 0x7ff0000000000000ul;
  if (exponent > bias)
    return sign | inf;
  const bool normal = exponent >= 1 - bias;
  // The input quantum is the binary64 subnormal unit, so only the divisor
  // needs a left shift, even for a subnormal output (0 or 925 bits).
  const uint drop = uint(max(k - mb, narrow ? 925 : 0));
  shift(denominator, drop, divisor, words);
  copy(remainder, numerator, words);
  ulong significand = 0;
  for (int bit = mb; bit >= 0; --bit) {
    shift(divisor, uint(bit), temporary, words);
    if (compare(remainder, temporary, words) >= 0) {
      subtract(remainder, temporary, words);
      significand |= 1ul << bit;
    }
  }
  shift(remainder, 1, temporary, words);
  const int halfway = compare(temporary, divisor, words);
  if (halfway > 0 || (halfway == 0 && (significand & 1)))
    ++significand;
  if (!normal)
    return sign | significand;
  if (significand == (1ul << (mb + 1))) {
    significand >>= 1;
    ++exponent;
  }
  if (exponent > bias)
    return sign | inf;
  return sign | (ulong(exponent + bias) << mb) |
         (significand & ((1ul << mb) - 1));
}
kernel void gaussian_exact(device const uchar* input [[buffer(0)]],
                           device uint* output [[buffer(1)]],
                           device const ulong* kx [[buffer(2)]],
                           device const ulong* ky [[buffer(3)]],
                           device uint* scratch [[buffer(4)]],
                           constant Arguments& args [[buffer(5)]],
                           uint item [[thread_position_in_grid]]) {
  if (item >= args.count)
    return;
  device uint* state = scratch + ulong(item) * lane_words;
  device uint* numerator = state + 8;
  device uint* denominator = numerator + words;
  device uint* x = denominator + words;
  device uint* y = x + words;
  device uint* product = y + words;
  device uint* value = product + words;
  device uint* term = value + words;
  device uint* temporary = term + words;
  device uint* divisor = temporary + words;
  device uint* remainder = divisor + words;
  if (args.initialize) {
    clear(state, lane_words);
    state[2] = 1;
  }
  const ulong index = args.begin + item;
  ulong coordinates[8];
  ulong linear = index;
  for (int axis = int(args.rank) - 1; axis >= 0; --axis) {
    coordinates[axis] = linear % args.shape[axis];
    linear /= args.shape[axis];
  }
  const ulong sign = 1ul << (args.narrow ? 31 : 63);
  const ulong inf = args.narrow ? 0x7f800000ul : 0x7ff0000000000000ul;
  const ulong quiet = 1ul << (args.narrow ? 22 : 51);
  for (ulong tap = args.tap_begin; tap < args.tap_end; ++tap) {
    const ulong j = tap / args.nx, i = tap % args.nx;
    const ulong wx = kx[i], wy = ky[j];
    if (!wx || !wy)
      continue;
    ulong xx = 0, yy = 0;
    // Static table-byte admission bounds the radii below 2^59; shape <=2^40.
    // These signed boundary expressions therefore fit long without int128.
    const bool inside_y =
        mapped(long(coordinates[args.y]) + long((args.ny - 1) / 2) - long(j),
               args.shape[args.y], args.boundary, yy);
    const bool inside_x =
        mapped(long(coordinates[args.x]) + long((args.nx - 1) / 2) - long(i),
               args.shape[args.x], args.boundary, xx);
    const bool inside = inside_y && inside_x;
    ulong raw = args.cval;
    const bool narrow = inside && args.narrow;
    if (inside) {
      ulong address = args.offset;
      for (uint axis = 0; axis < args.rank; ++axis) {
        const ulong at = axis == args.x   ? xx
                         : axis == args.y ? yy
                                          : coordinates[axis];
        address += (at - args.origin[axis]) * args.strides[axis];
      }
      raw = load(input, address, narrow);
    }
    if (args.identity) {
      state[5] = uint(raw);
      state[6] = uint(raw >> 32);
      continue;
    }
    set_value(x, wx, false);
    set_value(y, wy, false);
    multiply_trimmed(x, y, product);
    add(denominator, product, words);
    const uint mb = narrow ? 23 : 52;
    const ulong exponent_mask = narrow ? 0x7f800000ul : 0x7ff0000000000000ul;
    const bool negative = (raw >> (narrow ? 31 : 63)) != 0;
    const ulong magnitude = raw & ~(1ul << (narrow ? 31 : 63));
    const bool special = (raw & exponent_mask) == exponent_mask;
    const bool nan = special && (raw & ((1ul << mb) - 1));
    if (nan && !state[3] && !state[4]) {
      const ulong converted = raw | quiet;
      state[3] = uint(converted);
      state[4] = uint(converted >> 32);
    }
    if (special && !nan)
      state[1] |= negative ? 2 : 1;
    state[2] = state[2] && negative && !magnitude;
    if (special || state[1] || state[3] || state[4])
      continue;
    set_value(value, raw, narrow);
    multiply_trimmed(product, value, term);
    if (bool(state[0]) == negative)
      add(numerator, term, words);
    else if (compare(numerator, term, words) >= 0)
      subtract(numerator, term, words);
    else {
      subtract(term, numerator, words);
      copy(numerator, term, words);
      state[0] = negative;
    }
  }
  if (!args.finish)
    return;
  ulong result;
  if (args.identity)
    result = ulong(state[5]) | (ulong(state[6]) << 32);
  else if (state[3] || state[4])
    result = ulong(state[3]) | (ulong(state[4]) << 32);
  else if (state[1])
    result = state[1] == 3 ? inf | quiet : inf | (state[1] == 2 ? sign : 0);
  else if (state[2])
    result = sign;
  else
    result = rounded_ratio(numerator, denominator, state[0], args.narrow,
                           temporary, divisor, remainder);
  const ulong at = index * (args.narrow ? 1 : 2);
  output[at] = uint(result);
  if (!args.narrow)
    output[at + 1] = uint(result >> 32);
}
