"""Deterministic inputs and pinned resources for the FIL-19B comparison suite.

These functions define inputs only; no production operation or test harness runs.
Coordinates are logical (y, x), independent of physical image orientation.
"""
import hashlib
import json
from pathlib import Path
import struct


def f32(value):
    return struct.unpack("<f", struct.pack("<f", value))[0]


def hash32(value):
    value &= 0xffffffff
    value ^= value >> 16
    value = (value * 0x7feb352d) & 0xffffffff
    value ^= value >> 15
    value = (value * 0x846ca68b) & 0xffffffff
    return value ^ (value >> 16)


def rgb(y, x, channel, height, width, pattern):
    """Return the exact stored Float32 component of one comparison input."""
    if pattern == 0:
        return float(((x // 7) ^ (y // 5) ^ channel) & 1)
    if pattern == 1:
        return min(max((x * 3 - y * 2 + channel * 3 - width // 2) / 4 + .5, 0), 1)
    if pattern == 2:
        return (hash32((y * width + x) * 3 + channel) & 65535) / 65536
    if pattern == 3:
        threshold = f32(.15)
        raw = struct.unpack("<I", struct.pack("<f", threshold))[0]
        above = struct.unpack("<f", struct.pack("<I", raw + 1))[0]
        return (0., threshold, above, .5, 1.)[(x // 3 + y // 11 + channel) % 5]
    raise ValueError("unknown comparison pattern")


def comparison_cases():
    return [(h, w, pattern) for h, w in ((32, 32), (128, 128), (37, 53))
            for pattern in range(4)] + [(1024, 1024, 2), (4096, 4096, 1)]


def verify_pins(root=None):
    """Check the bundled original shader, lookup headers and license bytes."""
    root = Path(root) if root else Path(__file__).resolve().parents[4]
    production = root / "plugins/ops/05-filter/smaa_resources"
    oracle = root / "oracle/ops/filter/fil19/pinned"
    manifest = json.loads((production / "pin.json").read_text())
    for name, expected in manifest["files"].items():
        path = oracle / name if name == "SMAA.hlsl" else production / Path(name).name
        data = path.read_bytes()
        if len(data) != expected["bytes"] or hashlib.sha256(data).hexdigest() != expected["sha256"]:
            raise ValueError("pinned resource mismatch: " + name)
    return manifest["revision"]
