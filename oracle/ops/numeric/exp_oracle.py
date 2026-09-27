"""Generate a deterministic binary corpus with independent directed MPFR exp."""
import random
import struct
import sys
from math_oracle_support import direct


def bits(x):
    return struct.unpack('<I', struct.pack('<f', x))[0]


def main():
    rng = random.Random(404)
    rows = [bits(1), 0, 0x80000000, 0x7f800000, 0xff800000,
            0x7f800013, 0xff800042, 0x7fc12345, 1, 0x80000001,
            0x007fffff, 0x807fffff, bits(-90), bits(90), bits(81), bits(-81)]
    for edge in [-103.97208, -87.33655, 88.72284, -80, 80, 0.34657359, -0.34657359]:
        center = bits(edge)
        rows += list(range(center-16, center+17))
    rows += [bits(rng.uniform(-80, 80)) for _ in range(20000)]
    # Uniform raw words cover tiny inputs and the complete fallback domain.
    rows += [rng.getrandbits(32) for _ in range(256)]
    with open(sys.argv[1], 'wb') as output:
        for raw in rows:
            magnitude = raw & 0x7fffffff
            x = struct.unpack('<f', struct.pack('<I', raw))[0]
            if magnitude > 0x7f800000:
                expected = raw | 0x00400000
            elif x > 100:
                expected = 0x7f800000
            elif x < -110:
                expected = 0
            else:
                expected = direct('exp', 4, raw)
            output.write(struct.pack('<II', raw, expected))
    print(f'{len(rows)} MPFR-directed Float32 exp references written')


if __name__ == '__main__':
    main()
