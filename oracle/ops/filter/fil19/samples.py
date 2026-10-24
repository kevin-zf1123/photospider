"""Replayable native SMAA stage references and independent oracle assertions."""
import math

from oracle.ops.filter.common.core import bits, from_bits
from oracle.ops.filter.fil19.fixtures import rgb, verify_pins
from oracle.ops.filter.fil19.smaa import Reference, f32


def evaluate(image, *, guide=None, coordinates=None):
    """Evaluate H/W/C Float32 samples at explicit (y, x) coordinates.

    C is 1 (Gray) or 3 (RGB). An optional H/W scalar guide selects external
    luma. Returned luma, edge, weight and output arrays follow coordinate order.
    This computes independent reference values; it never invokes production.
    """
    if not image or not image[0] or not image[0][0]:
        raise ValueError("SMAA reference requires nonempty H/W/C")
    height, width, components = len(image), len(image[0]), len(image[0][0])
    if components not in (1, 3) or any(
            len(row) != width or any(len(pixel) != components for pixel in row)
            for row in image):
        raise ValueError("SMAA reference requires rectangular H/W/1 or H/W/3")
    if guide is not None and (len(guide) != height or
                              any(len(row) != width for row in guide)):
        raise ValueError("SMAA guide must match H/W")
    points = ([(y, x) for y in range(height) for x in range(width)]
              if coordinates is None else coordinates)
    if any(not (0 <= y < height and 0 <= x < width) for y, x in points):
        raise ValueError("SMAA reference coordinate outside image")
    reference = Reference(
        height, width, lambda y, x, c: f32(image[y][x][c]),
        guide=None if guide is None else lambda y, x: f32(guide[y][x]),
        components=components)
    try:
        return {
            "coordinates": [list(p) for p in points],
            "luma": [reference.luma(y, x) for y, x in points],
            "edge": [reference.edge(y, x) for y, x in points],
            "weight": [reference.weight(y, x) for y, x in points],
            "output": [[reference.output(y, x, c) for c in range(components)]
                       for y, x in points],
        }
    finally:
        # Reference's method caches otherwise retain every completed image.
        for method in (reference.luma, reference.edge, reference.weight):
            method.cache_clear()


def add_cases(suite):
    """Add analytic checks and staged vectors to the common oracle suite."""
    member = ["FIL-19B"]

    def require(condition, message):
        if not condition:
            raise AssertionError(message)

    def valid_stages(result):
        require(all(v in (0., 1.) for pair in result["edge"] for v in pair),
                "edge masks must be binary")
        for key in ("weight", "output"):
            require(all(math.isfinite(v) and 0 <= v <= 1
                        for row in result[key] for v in row),
                    key + " outside unit range")

    def vector(name, image, kwargs, expected=None, check=valid_stages):
        suite.vector(name, member, "smaa.evaluate", [image], kwargs,
                     expected=expected, check=check, dtype="float32",
                     level="NativeFloat32Staged")

    suite.case("smaa_pinned_resources", member, "PinnedResourceIdentity",
               verify_pins, check=lambda value: require(
                   value == "71c806a838bdd7d517df19192a20f0c61b3ca29d",
                   "unexpected SMAA revision"))
    for height, width in ((1, 1), (1, 9), (9, 1), (5, 7)):
        image = [[[-0.0] for _ in range(width)] for _ in range(height)]
        count = height * width
        vector(f"smaa_gray_zero_{height}x{width}", image, {}, {
            "luma": [-0.0] * count, "edge": [(0., 0.)] * count,
            "weight": [(0.,) * 4] * count, "output": [[-0.0]] * count})

    threshold = f32(.15)
    for delta, active, label in (
            (from_bits(bits(threshold, "float32") - 1, "float32"), 0., "below"),
            (threshold, 1., "equal"),
            (from_bits(bits(threshold, "float32") + 1, "float32"), 1., "above")):
        # Constant vertical step: local contrast is delta; >= includes equality.
        vector("smaa_edge_threshold_" + label,
               [[[0.], [delta], [delta]]], {"coordinates": [[0, 1]]},
               {"luma": [delta], "edge": [(active, 0.)]})

    vector("smaa_local_contrast_suppression", [[[0.], [.2], [1.]]],
           {"coordinates": [[0, 1]]}, {"edge": [(0., 0.)]})

    for pattern in range(4):
        height, width = 8, 11
        image = [[[rgb(y, x, c, height, width, pattern) for c in range(3)]
                  for x in range(width)] for y in range(height)]
        vector(f"smaa_rgb_pattern_{pattern}", image, {})
        guide = [[image[y][x][0] for x in range(width)] for y in range(height)]
        vector(f"smaa_external_pattern_{pattern}", image, {"guide": guide})

    image = [[[rgb(y, x, c, 8, 11, 2) for c in range(3)]
              for x in range(11)] for y in range(8)]
    vector("smaa_flat_external_guide_copy", image,
           {"guide": [[0.] * 11 for _ in range(8)]},
           {"edge": [(0., 0.)] * 88, "weight": [(0.,) * 4] * 88,
            "output": [pixel for row in image for pixel in row]})

    def smoothing(result):
        valid_stages(result)
        require(any(v > 0 for row in result["weight"] for v in row),
                "fixture must exercise nonzero blending weights")
        require(any(bits(a, "float32") != bits(b, "float32")
                    for actual, source in zip(result["output"],
                                              [p for row in image for p in row])
                    for a, b in zip(actual, source)),
                "fixture must exercise a changed output")

    vector("smaa_active_blend", image, {}, check=smoothing)
