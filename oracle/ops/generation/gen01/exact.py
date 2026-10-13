from __future__ import annotations
from typing import Iterable, Sequence


def constant_bits(values: Sequence[int], height: int, width: int):
    if height < 1 or width < 1 or not values:
        raise ValueError('positive extents required')
    return [[list(values) for _ in range(width)] for _ in range(height)]
