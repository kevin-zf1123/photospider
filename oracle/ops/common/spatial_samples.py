"""Shared spatial samples retained from the former native test wrapper."""

import math
import struct


def _float32(value):
    try:
        return struct.unpack("<f", struct.pack("<f", value))[0]
    except OverflowError:
        return math.copysign(math.inf, value)


HEIGHT = 5
WIDTH = 7
BATCH_AXES = (1, 1)
ABSOLUTE_TOLERANCE = _float32(1e-6)


def samples(mask=False):
    """Return row-major Float32-exact mask or RGBA values, with alpha 0.5."""
    channels = 1 if mask else 4
    return [
        (i % 9) / 8 if mask else 0.5 if i % 4 == 3 else (i % 11) / 4
        for i in range(HEIGHT * WIDTH * channels)
    ]


def expected_sample(values, row, column, channel, columns, mask=False):
    """Address a row-major expected image independently of its native layout."""
    channels = 1 if mask else 4
    return values[(row * columns + column) * channels + channel]


def matches(actual, expected):
    """Apply the original absolute-error acceptance rule."""
    difference = _float32(_float32(actual) - _float32(expected))
    return abs(difference) <= ABSOLUTE_TOLERANCE
