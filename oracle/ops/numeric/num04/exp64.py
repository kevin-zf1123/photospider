"""FP64 exp independent references: independent MPFR, range edges and no narrowing."""
import math
import random
import struct

from oracle.ops.numeric.common.accuracy import accepted
from oracle.ops.numeric.common.certified import reference


def bits(value):
    return int.from_bytes(struct.pack('>d', value), 'big')


def cases():
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
    return rows


def reference_cases():
    for word in cases():
        yield word, reference(0, 3, word, 0)


def accepted_case(word, actual, expected, profile='strict'):
    """Outside the admitted interval, require exact fallback/special bits."""
    x = struct.unpack('>d', word.to_bytes(8, 'big'))[0]
    return (accepted(actual, f'{expected:x}', 3, profile)
            if math.isfinite(x) and -80 <= x <= 80 else int(actual, 16) == expected)


DISTINCT_INPUTS = (bits(1), bits(1+2**-40))


def preserves_input_distinction(results):
    return results[DISTINCT_INPUTS[0]] != results[DISTINCT_INPUTS[1]]
