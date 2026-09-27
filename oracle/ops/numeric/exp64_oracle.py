"""FP64 exp public acceptance: independent MPFR, range edges and no narrowing."""
import math
import random
import struct
import subprocess
import sys

from accuracy_oracle import accepted
from certified_oracle import reference


def bits(value):
    return int.from_bytes(struct.pack('>d', value), 'big')


def main():
    profile = sys.argv[2] if len(sys.argv) > 2 else 'strict'
    rng = random.Random(406)
    rows = [bits(rng.uniform(-80, 80)) for _ in range(1024)]
    for x in [-1000, -745, -709, -100, -90, -80, -1, -2**-40, 0,
              2**-40, 1, 1+2**-40, 1+2**-30, 80, 81, 88, 100, 709, 710, 1000]:
        rows.extend(bits(y) for y in [math.nextafter(x, -math.inf), x,
                                     math.nextafter(x, math.inf)])
    rows += [0, 1 << 63, 1, (1 << 63) | 1,
             0x7fefffffffffffff, 0xffefffffffffffff,
             0x7ff0000000000000, 0xfff0000000000000,
             0x7ff0000000000042, 0xfff0000000000013,
             0x7ff8000000000051, 0xfff8000000000023]
    wanted = [reference(0, 3, word, 0) for word in rows]
    result = subprocess.run([sys.argv[1], profile, 'oracle'],
                            input=''.join(f'exp 3 3 {word:x} 0\n' for word in rows),
                            text=True, capture_output=True, check=True)
    answers = result.stdout.splitlines()
    assert len(answers) == len(rows), (len(answers), len(rows), result.stderr)
    maximum = 0
    by_input = {}
    for word, actual, expected in zip(rows, answers, wanted):
        assert accepted(actual, f'{expected:x}', 3, profile), (hex(word), actual, hex(expected))
        x = struct.unpack('>d', word.to_bytes(8, 'big'))[0]
        if math.isfinite(x) and -80 <= x <= 80:
            maximum = max(maximum, abs(int(actual, 16)-expected))
        else:
            assert int(actual, 16) == expected, ('strict fallback bits', hex(word), actual, hex(expected))
        by_input[word] = actual
    assert by_input[bits(1)] != by_input[bits(1+2**-40)], 'binary64 inputs were narrowed'
    print(f'{len(rows)} independent MPFR FP64 exp cases passed ({profile}); '
          f'admitted sample maximum_binary64_steps={maximum}; '
          'strict fallback/special bits and non-FP32 inputs passed')


if __name__ == '__main__':
    main()
