"""Deterministic directed-MPFR trig corpus, including normalized integer zeros."""
from pathlib import Path
import random
import struct
import sys
from certified_oracle import reference


def bits(x):
    return struct.unpack('<I', struct.pack('<f', x))[0]


def main():
    root = Path(sys.argv[1])
    root.mkdir(parents=True, exist_ok=True)
    rng = random.Random(406)
    for name, kind in [('sin', 2), ('cos', 3), ('sinpi', 5), ('cospi', 6),
                       ('sinc', 8), ('sincpi', 9)]:
        rows = [bits(.125), 0, 0x80000000, 0x7f800000, 0xff800000,
                0x7f800013, 0xff800042, 0x7fc12345, 1, 0x80000001,
                0x007fffff, 0x807fffff, bits(.25), bits(-.25), bits(1), bits(-1)]
        edges = [2**-120, .25, .5, .75, 1, 2, 3, 7, 16, 2**22, 2**23]
        for edge in edges:
            for sign in (1, -1):
                center = bits(edge*sign)
                rows += list(range(center-8, center+9))
        radius = .25 if kind in (5, 6) else 1
        rows += [bits(rng.uniform(-radius, radius)) for _ in range(4096)]
        if kind == 9:
            # Whole finite domain: integer/half-integer reduction, parity,
            # cancellation-free zero factors, large values and raw exponent bins.
            rows += [bits(rng.uniform(-1024, 1024)) for _ in range(4096)]
            for exponent in range(23):
                center = bits(2**exponent)
                rows += list(range(center-32, center+33))
        rows += [rng.getrandbits(32) for _ in range(256)]
        with (root / (name+'.bin')).open('wb') as output:
            for raw in rows:
                expected = reference(kind, 4, raw, 0)
                output.write(struct.pack('<II', raw, expected))
        print(f'{name}: {len(rows)} independent MPFR references', flush=True)


if __name__ == '__main__':
    main()
