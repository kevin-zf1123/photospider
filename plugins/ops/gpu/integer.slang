// Integer offsets address disjoint u32 limbs in the caller-defined scratch buffer.

void clear(uint64_t a, uint n) {
  for (uint i = 0; i < n; ++i)
    scratch[a + (i)] = 0;
}
void copy(uint64_t a, uint64_t b, uint n) {
  for (uint i = 0; i < n; ++i)
    scratch[a + (i)] = scratch[b + (i)];
}
void power(uint64_t a, uint e, uint n) {
  clear(a, n);
  scratch[a + (e / 32)] = 1u << (e % 32);
}
void add(uint64_t a, uint64_t b, uint n) {
  uint64_t carry = 0;
  for (uint i = 0; i < n; ++i) {
    const uint64_t v = uint64_t(scratch[a + (i)]) + scratch[b + (i)] + carry;
    scratch[a + (i)] = uint(v);
    carry = v >> 32;
  }
}
void subtract(uint64_t a, uint64_t b, uint n) {
  uint64_t borrow = 0;
  for (uint i = 0; i < n; ++i) {
    const uint64_t v = uint64_t(scratch[b + (i)]) + borrow;
    borrow = uint64_t(scratch[a + (i)]) < v;
    scratch[a + (i)] = uint(uint64_t(scratch[a + (i)]) - v);
  }
}
int compare(uint64_t a, uint64_t b, uint n) {
  for (uint i = n; i-- > 0;)
    if (scratch[a + (i)] != scratch[b + (i)])
      return scratch[a + (i)] > scratch[b + (i)] ? 1 : -1;
  return 0;
}
uint length(uint64_t a, uint n) {
  while (n != 0 && scratch[a + (n - 1)] == 0)
    --n;
  return n;
}
void multiply(uint64_t a, uint64_t b, uint64_t destination,
              uint n) {
  const uint na = length(a, n), nb = length(b, n);
  clear(destination, n);
  for (uint i = 0; i < na; ++i) {
    uint64_t carry = 0;
    for (uint j = 0; j < nb && i + j < n; ++j) {
      // (2^32-1)^2 + two (2^32-1) terms fits exactly in uint64_t.
      const uint64_t v = uint64_t(scratch[a + (i)]) * scratch[b + (j)] + scratch[destination + (i + j)] + carry;
      scratch[destination + (i + j)] = uint(v);
      carry = v >> 32;
    }
    if (i + nb < n)
      scratch[destination + (i + nb)] = uint(carry);
  }
}
void times(uint64_t a, uint factor, uint n) {
  uint64_t carry = 0;
  for (uint i = 0; i < n; ++i) {
    const uint64_t v = uint64_t(scratch[a + (i)]) * factor + carry;
    scratch[a + (i)] = uint(v);
    carry = v >> 32;
  }
}
void shift(uint64_t a, uint bits, uint64_t b, uint n) {
  clear(b, n);
  const uint words = bits / 32, rest = bits % 32;
  for (uint i = 0; i + words < n; ++i) {
    scratch[b + (i + words)] |= scratch[a + (i)] << rest;
    if (rest != 0 && i + words + 1 < n)
      scratch[b + (i + words + 1)] |= scratch[a + (i)] >> (32 - rest);
  }
}
