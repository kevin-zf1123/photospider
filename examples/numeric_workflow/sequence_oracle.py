"""Manual independent Fraction/IEEE oracle; run with the public workflow binary."""

import random
import struct
import subprocess
import sys
from fractions import Fraction


def value(bits):
    return struct.unpack(">d", struct.pack(">Q", bits))[0]


def ieee_round(number, width, negative_zero):
    fraction, bias = (23, 127) if width == 32 else (52, 1023)
    sign = number < 0 or (number == 0 and negative_zero)
    number = abs(number)
    if not number:
        return int(sign) << (width - 1)
    n, d = number.numerator, number.denominator
    exponent = n.bit_length() - d.bit_length()
    power = Fraction(2) ** exponent
    if number < power:
        exponent -= 1
    quantum = Fraction(2) ** max(exponent - fraction, 1 - bias - fraction)
    scaled = number / quantum
    quotient, remainder = divmod(scaled.numerator, scaled.denominator)
    twice = 2 * remainder
    quotient += twice > scaled.denominator or (
        twice == scaled.denominator and quotient % 2
    )
    rounded = quotient * quantum
    if rounded >= Fraction(2) ** (bias + 1):
        return None
    if rounded < Fraction(2) ** (1 - bias):
        payload = int(rounded / (Fraction(2) ** (1 - bias - fraction)))
    else:
        exponent = rounded.numerator.bit_length() - rounded.denominator.bit_length()
        if rounded < Fraction(2) ** exponent:
            exponent -= 1
        mantissa = int(rounded / (Fraction(2) ** (exponent - fraction)))
        payload = ((exponent + bias) << fraction) | (mantissa - (1 << fraction))
    return (int(sign) << (width - 1)) | payload


def main():
    rng = random.Random(20260914)
    special = [
        0, 1 << 63, 1, (1 << 63) | 1, 0x0010000000000000,
        0x3FF0000000000000, 0x3FF0000020000001,
        0x7FEFFFFFFFFFFFFF, 0xFFEFFFFFFFFFFFFF,
    ]
    cases = []
    for serial in range(240):
        a = special[serial % len(special)] if serial < 81 else rng.getrandbits(64)
        b = special[(serial // len(special)) % len(special)] if serial < 81 else rng.getrandbits(64)
        if ((a >> 52) & 2047) == 2047 or ((b >> 52) & 2047) == 2047:
            continue
        count = rng.choice([1, 2, 3, 7, 1048576])
        index = rng.randrange(count)
        for kind in ("linspace", "arange"):
            x, y = Fraction(value(a)), Fraction(value(b))
            if not index:
                exact = x
                minus_zero = bool(a >> 63)
            elif kind == "linspace":
                exact = ((count - 1 - index) * x + index * y) / (count - 1)
                minus_zero = bool(b >> 63) if index == count - 1 else bool((a & b) >> 63)
            else:
                exact = x + index * y
                minus_zero = bool((a & b) >> 63)
            for width in (32, 64):
                expected = ieee_round(exact, width, minus_zero)
                cases.append((f"{kind} float{width} {a:x} {b:x} {count} {index}", expected))
    run = subprocess.run([sys.argv[1], "--oracle", *sys.argv[2:]], input="\n".join(row for row, _ in cases) + "\n",
                         text=True, capture_output=True, check=True)
    actual = run.stdout.splitlines()
    assert len(actual) == len(cases), (len(actual), len(cases), run.stderr)
    for (row, expected), result in zip(cases, actual):
        if expected is None:
            assert result == "error 3", (row, expected, result)
        else:
            assert result == f"{expected:x}", (row, f"{expected:x}", result)
    print(f"independent Fraction/IEEE public-workflow oracle: {len(cases)} cases passed")


if __name__ == "__main__":
    main()
