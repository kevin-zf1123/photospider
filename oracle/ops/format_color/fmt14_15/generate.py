#!/usr/bin/env python3
"""Independent FMT-14/15 finite arithmetic oracle (Python 3.9+, mpmath).

Uses Fraction and integer ties-to-even rounding, with the independently authored
FMT-10 geometry and FMT-11 OKLab oracle. The OKLab forward stage is evaluated at
180 and 360 decimal digits and must give the same stored bits; this stability
check is not a universal proof of transcendental rounding. Every inverse stage,
dyadic bisection and rational mapping is exact. No production implementation is
loaded. --output writes fixtures for a temporary validator; --check compares an
existing fixture file without rewriting it.
"""
from __future__ import annotations

import argparse
from fractions import Fraction as F
import importlib.util
from pathlib import Path
import random

import mpmath as mp


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


ROOT = Path(__file__).resolve().parent.parent
GEOMETRY = module("fmt10_oracle", ROOT / "fmt10/generate.py")
MODEL = module("fmt11_oracle", ROOT / "fmt11/generate_oracles.py")
BASES = [GEOMETRY.PRESETS[0], (.5, .25, 0., 1., 0., .5, .25, .5)]
MATRICES = [GEOMETRY.basis(b) for b in BASES]
INVERSES = [GEOMETRY.inverse(m) for m in MATRICES]
decode, rounded = GEOMETRY.decode, GEOMETRY.round_bits


def finite(word, narrow):
    shift, mask = (23, 255) if narrow else (52, 2047)
    return (word >> shift) & mask != mask


def stored(values, narrow):
    words = [rounded(v, narrow) for v in values]
    if not all(finite(w, narrow) for w in words):
        raise OverflowError
    return words


def reduce(words, narrow, basis):
    """Return output and all bisection acceptance decisions, in order."""
    x = [decode(w, narrow) for w in words]
    inside = lambda v: all(0 <= a <= 1 for a in v)
    if inside(x):
        return words, []
    xyz = [decode(w, narrow) for w in stored(GEOMETRY.mv(MATRICES[basis], x), narrow)]
    def forward(digits):
        with mp.workdps(digits):
            return stored([MODEL.to_fraction(v) for v in MODEL.transcendental(4, xyz)], narrow)
    lab_words = forward(180)
    assert lab_words == forward(360), "unstable forward OKLab oracle"
    lab = [decode(w, narrow) for w in lab_words]
    if lab[0] <= 0 or lab[0] >= 1:
        return stored([F(int(lab[0] >= 1))] * 3, narrow), []
    def candidate(t):
        scaled = [lab[0]] + [decode(rounded(t * a, narrow), narrow) for a in lab[1:]]
        xyz = [decode(w, narrow) for w in stored(MODEL.rational(5, scaled), narrow)]
        return stored(GEOMETRY.mv(INVERSES[basis], xyz), narrow)
    zero, unit = 0, rounded(F(1), narrow)
    accepted = candidate(F(0))
    if not inside([decode(w, narrow) for w in accepted]):
        return [zero if decode(w, narrow) < 0 else unit if decode(w, narrow) > 1 else w
                for w in accepted], []
    trial = candidate(F(1))
    if inside([decode(w, narrow) for w in trial]):
        return trial, []
    low, high, decisions = F(0), F(1), []
    for _ in range(32 if narrow else 64):
        middle = (low + high) / 2
        trial = candidate(middle)
        good = inside([decode(w, narrow) for w in trial])
        decisions.append(good)
        if good:
            low, accepted = middle, trial
        else:
            high = middle
    return accepted, decisions


def evaluate(kind, narrow, basis, words, gain, white, knee, black, peak,
             extended, signed, to_nits):
    x = [decode(w, narrow) for w in words]
    if kind == 0:
        return [0 if a < 0 else rounded(F(1), narrow) if a > 1 else w
                for a, w in zip(x, words)]
    if kind == 1:
        return reduce(words, narrow, basis)[0]
    if kind == 3:
        return [rounded(F(int(any(a < 0 or a > 1 for a in x))), narrow), 0, 0]
    y = sum((w * a for w, a in zip(MATRICES[basis][3:6], x)), F(0))
    if kind == 2:
        if y <= 0 or y >= 1:
            return stored([F(int(y >= 1))] * 3, narrow)
        d = [a - y for a in x]
        q = max(a / (1 - y) if a >= 0 else -a / y for a in d)
        if q <= knee:
            return words
        u, v = q - knee, 1 - knee
        scale = (knee + v * u / (u + v)) / q
        return stored([y + a * scale for a in d], narrow)
    if kind == 4:
        if y < 0 and not signed:
            raise ValueError("negative luminance")
        z = gain * abs(y)
        scale = gain * (1 + z / (white * white) if extended else 1) / (1 + z)
        return stored([scale * a for a in x], narrow)
    return stored([black + (peak - black) * a if to_nits else (a - black) / (peak - black)
                   for a in x], narrow)


def fixtures():
    rng = random.Random(1401501)
    rows = ["# kind narrow basis gain white knee black peak extended signed to_nits input[3] expected[3] error"]
    for narrow in (False, True):
        max_word = 0x7f7fffff if narrow else 0x7fefffffffffffff
        base_words = [
            [GEOMETRY.raw(a, narrow) for a in triple]
            for triple in [(-0., 1.25, -.25), (1.5, .25, 0.), (.25, .25, .25),
                           (3., 3., 3.), (2., -1., 0.), (-1., -1., -1.),
                           (0., 0., 0.), (1., 1., 1.), (4., 4., 4.)]
        ]
        base_words += [[1, 1, 1], [max_word, max_word, max_word],
                       [max_word, max_word | (1 << (31 if narrow else 63)), 1]]
        for kind in range(6):
            samples = list(base_words)
            samples += [[GEOMETRY.raw(rng.uniform(-2, 5), narrow) for _ in range(3)]
                        for _ in range(3 if kind == 1 else 52)]
            for index, words in enumerate(samples):
                basis = 0 if kind == 1 else index % 2
                gain = (1., 2., .7, 1e200)[index % 4]
                white = (1., 2., 1e-100)[index % 3]
                knee = (.5, .8, 0.)[index % 3]
                black, peak = (1., 101.) if index % 2 else (.1, 203.)
                extended, signed, to_nits = index % 3 == 1, index % 2 == 0, index % 2 == 1
                args = [F(v) for v in (gain, white, knee, black, peak)]
                error = 0
                try:
                    out = evaluate(kind, narrow, basis, words, *args, extended, signed, to_nits)
                except ValueError:
                    out, error = [0, 0, 0], 1
                except OverflowError:
                    out, error = [0, 0, 0], 2
                fields = [str(kind), str(int(narrow)), str(basis)]
                fields += [str(v) for v in (gain, white, knee, black, peak)]
                fields += [str(int(v)) for v in (extended, signed, to_nits)]
                fields += [format(w, "x") for w in words + out]
                fields += [str(error)]
                rows.append(" ".join(fields))
    return "\n".join(rows) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    data = fixtures()
    if args.check:
        if args.output.read_text() != data:
            raise SystemExit("oracle mismatch")
    else:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(data)
    print(f"{len(data.splitlines()) - 1} independent mapping fixtures")


if __name__ == "__main__":
    main()
