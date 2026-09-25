#!/usr/bin/env python3
"""Generate exact finite arithmetic cases with Python's independent Fraction."""
from fractions import Fraction
from pathlib import Path
import argparse
import random


def cases():
    rng = random.Random(0x464D543131)
    pairs = []
    for bits in [1, 2, 63, 64, 65, 127, 128, 129, 255, 256, 511, 512,
                 1023, 1074, 2048, 4096, 8191]:
        for _ in range(4):
            values = [rng.getrandbits(bits) | 1 for _ in range(4)]
            pairs.append((Fraction(values[0], values[1]),
                          Fraction(-values[2], values[3])))
    for _ in range(160):
        bits = rng.randrange(1, 769)
        values = [rng.getrandbits(bits) | 1 for _ in range(4)]
        pairs.append((Fraction(values[0], values[1]),
                      Fraction((-1 if rng.randrange(2) else 1) * values[2],
                               values[3])))
    for a, b in pairs:
        for op, result in [("+", a + b), ("-", a - b), ("*", a * b),
                           ("/", a / b)]:
            yield op, a, b, result
    for bits in [0, 1, 63, 64, 65, 127, 128, 129, 255, 1024, 8192, 16000]:
        a, b = Fraction(3 << bits), Fraction(5 << bits)
        for op, result in [("+", a + b), ("-", a - b), ("/", a / b)]:
            yield op, a, b, result
    for bits in [64, 65, 128, 129, 256, 1024, 4096, 16382]:
        a, b = Fraction(1 << bits), Fraction((1 << bits) - 1)
        yield "/", a, b, a / b
        yield "-", a, b, a - b
        a, b = Fraction((1 << bits) + 1), Fraction((1 << (bits - 1)) + 1)
        yield "/", a, b, a / b
    for a, b in [(Fraction(0), Fraction(1)), (Fraction(-1), Fraction(1)),
                 (Fraction(7, 3), Fraction(7, 3))]:
        for op, result in [("+", a+b), ("-", a-b), ("*", a*b), ("/", a/b)]:
            yield op, a, b, result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=Path(__file__).resolve().parents[2]
                        / "tests/fixtures/fmt11/rational_oracles.txt")
    args = parser.parse_args()
    rows = []
    for op, a, b, expected in cases():
        rows.append(" ".join([op] + [format(x, "x") for v in (a, b, expected)
                                      for x in (v.numerator, v.denominator)]))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text("\n".join(rows) + "\n")
    print(f"wrote {len(rows)} exact Fraction arithmetic cases")


if __name__ == "__main__":
    main()
