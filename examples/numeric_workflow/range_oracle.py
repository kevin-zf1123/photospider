"""Independent exact rational and raw-bit NUM-06 public-workflow oracle."""
import math
import random
import subprocess
import sys
from comparison_oracle import number
from sequence_oracle import ieee_round


def reference(operation, dtype, bits):
    values = [number(x, dtype) for x in bits]
    x, lower, upper = values[:3]
    quiet = (1 << (22 if dtype == 4 else 51)) if dtype in (3, 4) else 0
    if operation == "clamp":
        if lower is None or upper is None or lower > upper:
            return "error"
        if x is None:
            return f"{bits[0] | quiet:x}"
        return f"{bits[1] if x < lower else bits[2] if x > upper else bits[0]:x}"
    if any(v is None or v in (math.inf, -math.inf) for v in values[1:]) or lower >= upper:
        return "error"
    if x is None:
        return f"{bits[0] | quiet:x}"
    target_lower, target_upper = values[3:]
    if target_lower == target_upper or x == lower:
        return f"{bits[3]:x}"
    if x == upper:
        return f"{bits[4]:x}"
    width = 32 if dtype == 4 else 64
    infinity = 0x7f800000 if width == 32 else 0x7ff0000000000000
    if x in (math.inf, -math.inf):
        return f"{infinity | (int((x < 0) != (target_upper < target_lower)) << (width-1)):x}"
    exact = target_lower + (x-lower)*(target_upper-target_lower)/(upper-lower)
    result = ieee_round(exact, width, False)
    if result is None:
        result = infinity | (int(exact < 0) << (width-1))
    return f"{result:x}"


def main():
    rng = random.Random(606)
    rows, expected = [], []

    def add(operation, dtype, bits):
        rows.append(f"{operation} {dtype} " + " ".join(f"{b:x}" for b in bits) + "\n")
        expected.append(reference(operation, dtype, bits))

    for dtype in (1, 2, 3, 4):
        if dtype == 1:
            special = [0, 1, 127, 128, 254, 255]
        elif dtype == 2:
            special = [0, 1, 2**53, 2**53+1, 2**63-1, 2**63, 2**64-1]
        else:
            sign = 1 << (31 if dtype == 4 else 63)
            inf = 0x7f800000 if dtype == 4 else 0x7ff0000000000000
            one = 0x3f800000 if dtype == 4 else 0x3ff0000000000000
            special = [0, sign, 1, sign|1, one, sign|one, inf-1, sign|(inf-1), inf, sign|inf, inf|1, sign|inf|0x1234]
        for _ in range(240):
            add("clamp", dtype, [rng.choice(special) for _ in range(3)] + [0,0])
        if dtype not in (3, 4):
            continue
        finite = [b for b in special if number(b, dtype) is not None and number(b, dtype) not in (math.inf,-math.inf)]
        for _ in range(200):
            lower, upper = sorted(rng.sample(finite, 2), key=lambda b: number(b, dtype))
            for x in [lower, upper, rng.choice(special)]:
                add("remap_range", dtype, [x, lower, upper, rng.choice(finite), rng.choice(finite)])
        width = 32 if dtype == 4 else 64
        for _ in range(320):
            bits = []
            while len(bits) < 5:
                raw = rng.getrandbits(width)
                value = number(raw, dtype)
                if value is not None and value not in (math.inf,-math.inf):
                    bits.append(raw)
            if number(bits[1], dtype) > number(bits[2], dtype):
                bits[1], bits[2] = bits[2], bits[1]
            add("remap_range", dtype, bits)
        # Invalid bounds must outrank input NaN; constant targets retain -0.
        add("remap_range", dtype, [inf|1, inf, one, 0, one])
        add("remap_range", dtype, [inf, 0, one, sign, 0])
        add("remap_range", dtype, [one, 0, one, sign, 0])
    for dtype, one, two, three, normal, maximum, sign in [
            (3,0x3ff0000000000000,0x4000000000000000,0x4008000000000000,0x0010000000000000,0x7fefffffffffffff,1<<63),
            (4,0x3f800000,0x40000000,0x40400000,0x00800000,0x7f7fffff,1<<31)]:
        for bits in [[one,0,two,0,1],[sign|one,0,two,0,1],[three,0,two,0,1],
                     [one,0,two,normal-1,normal],[one,0,two,two-1,two],
                     [three,0,two,maximum-1,maximum],[three-1,0,two,maximum-1,maximum],
                     [three+1,0,two,maximum-1,maximum],
                     [1,sign|maximum,maximum,sign|maximum,maximum],
                     [maximum,sign|maximum,sign|(maximum-1),sign|maximum,maximum]]:
            add("remap_range",dtype,bits)
    output = subprocess.run([sys.argv[1],sys.argv[2] if len(sys.argv)>2 else "_strict","oracle"],
                            input="".join(rows),text=True,capture_output=True,check=True)
    actual = output.stdout.splitlines()
    if len(actual) != len(expected): raise AssertionError((len(actual),len(expected),output.stderr))
    for row,want,got in zip(rows,expected,actual):
        if got != want: raise AssertionError((row.strip(),want,got))
    print(f"independent exact range/Fraction oracle: {len(expected)} cases passed")


if __name__ == "__main__":
    main()
