#include <metal_stdlib>
using namespace metal;

void clear(device uint* a, uint n) {
  for (uint i = 0; i < n; ++i)
    a[i] = 0;
}
void copy(device uint* a, device const uint* b, uint n) {
  for (uint i = 0; i < n; ++i)
    a[i] = b[i];
}
void power(device uint* a, uint e, uint n) {
  clear(a, n);
  a[e / 32] = 1u << (e % 32);
}
void add(device uint* a, device const uint* b, uint n) {
  ulong carry = 0;
  for (uint i = 0; i < n; ++i) {
    const ulong v = ulong(a[i]) + b[i] + carry;
    a[i] = uint(v);
    carry = v >> 32;
  }
}
void subtract(device uint* a, device const uint* b, uint n) {
  ulong borrow = 0;
  for (uint i = 0; i < n; ++i) {
    const ulong v = ulong(b[i]) + borrow;
    borrow = ulong(a[i]) < v;
    a[i] = uint(ulong(a[i]) - v);
  }
}
int compare(device const uint* a, device const uint* b, uint n) {
  for (uint i = n; i-- > 0;)
    if (a[i] != b[i])
      return a[i] > b[i] ? 1 : -1;
  return 0;
}
uint length(device const uint* a, uint n) {
  while (n && !a[n - 1])
    --n;
  return n;
}
void multiply(device const uint* a, device const uint* b, device uint* out,
              uint n) {
  const uint na = length(a, n), nb = length(b, n);
  clear(out, n);
  for (uint i = 0; i < na; ++i) {
    ulong carry = 0;
    for (uint j = 0; j < nb && i + j < n; ++j) {
      // (2^32-1)^2 + two (2^32-1) terms fits exactly in ulong.
      const ulong v = ulong(a[i]) * b[j] + out[i + j] + carry;
      out[i + j] = uint(v);
      carry = v >> 32;
    }
    if (i + nb < n)
      out[i + nb] = uint(carry);
  }
}
void times(device uint* a, uint factor, uint n) {
  ulong carry = 0;
  for (uint i = 0; i < n; ++i) {
    const ulong v = ulong(a[i]) * factor + carry;
    a[i] = uint(v);
    carry = v >> 32;
  }
}
void shift(device const uint* a, uint bits, device uint* b, uint n) {
  clear(b, n);
  const uint words = bits / 32, rest = bits % 32;
  for (uint i = 0; i + words < n; ++i) {
    b[i + words] |= a[i] << rest;
    if (rest && i + words + 1 < n)
      b[i + words + 1] |= a[i] >> (32 - rest);
  }
}
