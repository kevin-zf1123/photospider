"""Independent exact Fraction prefix/rectangle sums for NUM-13."""
import itertools
import functools
import random
import subprocess
import sys
from reduction_oracle import reference_group


def cases():
    rng = random.Random(1301)
    for integral in (0, 1):
        shapes = [(3,), (2, 3), (3, 2, 2)] if not integral else [(2, 3), (2, 2, 3)]
        for shape in shapes:
            selections = itertools.combinations(range(len(shape)), 2 if integral else 1)
            for axes in selections:
                count = 1
                for extent in shape:
                    count *= extent
                for source in (1, 2, 3, 4):
                    destinations = (1, 2) if source in (1, 2) else (3, 4)
                    if source == 1:
                        pools = [[0, 1, 127, 255]]
                    elif source == 2:
                        pools = [[0, 1, (1 << 64)-1, (1 << 63)-1, 1 << 63], [1, 2, 3]]
                    elif source == 3:
                        pools = [[0, 1 << 63, 1, (1 << 63)+1, 0x3ff0000000000000, 0xbff0000000000000,
                                  0x7fefffffffffffff, 0xffefffffffffffff],
                                 [0x7ff0000000000000, 0xfff0000000000000, 0x7ff0000000000042, 0xfff8000000000013, 0]]
                    else:
                        pools = [[0, 1 << 31, 1, (1 << 31)+1, 0x3f800000, 0xbf800000, 0x7f7fffff, 0xff7fffff],
                                 [0x7f800000, 0xff800000, 0x7f800042, 0xffc00013, 0]]
                    for pool in pools:
                        bits = [rng.choice(pool) for _ in range(count)]
                        for destination in destinations:
                            out = [n+(i in axes) for i, n in enumerate(shape)]
                            for point in itertools.product(*(range(n) for n in out)):
                                yield integral, source, destination, shape, axes, point, bits
    # Complete rectangle reference covers carry across rows and separate planes.
    for shape, axes in [((5, 7), (0, 1)), ((3, 2, 4), (0, 2))]:
        count = 1
        for extent in shape:
            count *= extent
        for source, pool in [(2, [0, 1, 2, (1 << 64)-1]),
                             (3, [0x4340000000000000, 0x3ff0000000000000, 0xc340000000000000, 0x8000000000000000]),
                             (3, [0x7fefffffffffffff, 0xffefffffffffffff, 0x7ff0000000000042, 0xfff0000000000013])]:
            bits = [pool[i % len(pool)] for i in range(count)]
            out = [n+(i in axes) for i, n in enumerate(shape)]
            for point in itertools.product(*(range(n) for n in out)):
                yield 1, source, source, shape, axes, point, bits
    for size in (63, 64, 65, 129):
        for source, maximum, negative in ((2, (1 << 63)-1, (1 << 64)-1),
                                           (3, 0x7fefffffffffffff, 0xffefffffffffffff)):
            bits = [maximum, maximum] + [0]*(size-3) + [negative]
            for stop in (0, 1, 2, size-1, size):
                yield 0, source, source, (size,), (0,), (stop,), bits


def expected(case):
    integral, source, destination, shape, axes, point, bits = case
    terms = []
    for index, coordinate in enumerate(itertools.product(*(range(n) for n in shape))):
        if all(coordinate[i] < point[i] if i in axes else coordinate[i] == point[i]
               for i in range(len(shape))):
            terms.append(bits[index])
    return reference_group('sum', source, destination, 0, terms) if terms else 0


@functools.lru_cache(None)
def whole_overflow(integral, source, destination, shape, axes, bits):
    if source not in (1, 2):
        return False
    out = tuple(n + (i in axes) for i, n in enumerate(shape))
    return any(expected((integral, source, destination, shape, axes, point, bits)) == 'overflow'
               for point in itertools.product(*(range(n) for n in out)))


def main():
    rows = list(cases())
    encoded = []
    for integral, source, destination, shape, axes, point, bits in rows:
        lists = [','.join(map(str, values)) for values in (shape, axes, point)]
        encoded.append(f'{integral} {source} {destination} ' + ' '.join(lists) + ' ' + ' '.join(f'{x:x}' for x in bits))
    result = subprocess.run([sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else 'strict', 'oracle'],
                            input='\n'.join(encoded)+'\n', text=True, capture_output=True, check=True)
    answers = result.stdout.splitlines()
    assert len(answers) == len(rows), (len(answers), len(rows), result.stderr)
    for index, (case, actual) in enumerate(zip(rows, answers)):
        integral, source, destination, shape, axes, point, bits = case
        wanted = "overflow" if whole_overflow(integral, source, destination, shape, axes, tuple(bits)) else expected(case)
        assert actual == (wanted if isinstance(wanted, str) else f'{wanted:x}'), (index, case, wanted, actual)
    print(f'{len(rows)} independent exact scan cases passed ({sys.argv[2] if len(sys.argv)>2 else "strict"})')


if __name__ == '__main__':
    main()
