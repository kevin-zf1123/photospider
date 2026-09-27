"""Independent raw-IEEE/Fraction oracle through Compiler/ExecutionContext."""
import math
import random
import subprocess
import sys
from fractions import Fraction


def number(bits, dtype):
    if dtype == 1:
        return bits
    if dtype == 2:
        return bits - (1 << 64) if bits >> 63 else bits
    fraction, exp_bits, bias = (52, 11, 1023) if dtype == 3 else (23, 8, 127)
    sign = -1 if bits >> (fraction + exp_bits) else 1
    exponent = (bits >> fraction) & ((1 << exp_bits) - 1)
    mantissa = bits & ((1 << fraction) - 1)
    if exponent == (1 << exp_bits) - 1:
        return None if mantissa else sign * math.inf
    shift = (exponent - bias if exponent else 1 - bias) - fraction
    exact = Fraction(mantissa + ((1 << fraction) if exponent else 0))
    return sign * exact * (Fraction(2) ** shift)


def relation(operation, a, b, absolute, relative):
    if a is None or b is None:
        return int(operation == "not_equal")
    if operation == "is_close":
        if a in (math.inf, -math.inf) or b in (math.inf, -math.inf):
            return int(a == b)
        return int(abs(a - b) <= absolute + relative * max(abs(a), abs(b)))
    return int({"equal": lambda: a == b, "not_equal": lambda: a != b,
                "less": lambda: a < b, "less_equal": lambda: a <= b,
                "greater": lambda: a > b, "greater_equal": lambda: a >= b}[operation]())


def main():
    generator = random.Random(707)
    records, expected = [], []

    def add(operation, dtype, a, b, absolute=0, relative=0):
        records.append(f"{operation} {dtype} {a:x} {b:x} {absolute:x} {relative:x}\n")
        expected.append(relation(operation, number(a, dtype), number(b, dtype),
                                 number(absolute, 3), number(relative, 3)))

    for dtype in (1, 2, 3, 4):
        if dtype == 1:
            values = [0, 1, 2, 127, 128, 254, 255]
        elif dtype == 2:
            values = [0, 1, 2**53, 2**53+1, 2**63-1, 2**63, 2**64-1]
        else:
            fraction, exp_bits = (52, 11) if dtype == 3 else (23, 8)
            sign = 1 << (fraction + exp_bits)
            inf = ((1 << exp_bits)-1) << fraction
            values = [0, sign, 1, sign|1, inf-1, sign|(inf-1), inf, sign|inf,
                      inf|1, sign|inf|7, inf|(1 << (fraction-1))|3]
        for a in values:
            for b in values:
                for operation in ("equal", "not_equal", "less", "less_equal", "greater", "greater_equal"):
                    add(operation, dtype, a, b)
                if dtype in (3, 4):
                    add("is_close", dtype, a, b, 0, 0x3ff8000000000000)
        if dtype in (3, 4):
            width = 64 if dtype == 3 else 32
            for _ in range(192):
                a, b = generator.getrandbits(width), generator.getrandbits(width)
                absolute = generator.choice([0, 1, 0x3fd0000000000000, 0x7fefffffffffffff])
                relative = generator.choice([0, 1, 0x3fe0000000000000, 0x3ff0000000000000, 0x4000000000000000])
                add("is_close", dtype, a, b, absolute, relative)
            for relative in (0x3fe0000000000000, 0x3ff0000000000000, 0x4000000000000000):
                add("is_close", dtype, 1, 0, 0, relative)
    maximum = 0x7fefffffffffffff
    bases = [0, 1, 0x000fffffffffffff, 0x0010000000000000, 0x3ff0000000000000, maximum]
    values = bases + [v | (1 << 63) for v in bases]
    for a in values:
        for b in values:
            for absolute, relative in [(0, maximum), (maximum, maximum), (1, 1), (maximum, 0)]:
                add("is_close", 3, a, b, absolute, relative)
    extremes = random.Random(707_4197)
    tolerances = bases + [0x3fefffffffffffff, 0x3ff0000000000001]
    for _ in range(512):
        a = extremes.randrange(maximum + 1) | (extremes.randrange(2) << 63)
        b = extremes.randrange(maximum + 1) | (extremes.randrange(2) << 63)
        add("is_close", 3, a, b, extremes.choice(tolerances), extremes.choice(tolerances))
    output = subprocess.run([sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else "_strict", "oracle"],
                            input="".join(records), text=True, capture_output=True, check=True)
    actual = [int(line) for line in output.stdout.splitlines()]
    if len(actual) != len(expected):
        raise AssertionError((len(actual), len(expected), output.stderr))
    for row, want, got in zip(records, expected, actual):
        if want != got:
            raise AssertionError((row.strip(), want, got))
    print(f"independent exact comparison/Fraction oracle: {len(expected)} cases passed")


if __name__ == "__main__":
    main()
