# Strict grading oracles

These independent references accompany [GRD specifications](../../../docs/built-in_ops/07-grade/adjustments.md). They cover 100 member identities through reusable scalar, pixel and small-array functions. Production GRD computation uses native FP32/FP64; these oracles provide the strictly rounded comparison values.

## Run

From the repository root:

```sh
python3 oracle/ops/07-grade/run.py --check
python3 oracle/ops/07-grade/run.py --member GRD-11C
python3 oracle/ops/07-grade/run.py
```

The first command verifies authored analytic anchors and rounding/selection properties of the reference. The other commands print JSON lines of strict reference values. They write no reports or fixtures. Python's standard library and an LP64 MPFR 4.2 or later shared library are required. Set `PHOTOSPIDER_ORACLE_MPFR` to its absolute library path when automatic discovery is unavailable. Running the oracle does not install packages or download data.

`catalog.py` maps each member ID to a callable in `REFERENCES` and one representative call in `SAMPLES`. Import the callable to evaluate additional input values. Members that operate on image components are represented by one selected scalar/pixel observation; spatial/statistical references accept small nested arrays. This is a mathematical API, not the runtime Result/Region API. The scalar reference for GRD-01B, for example, receives the source component and its matching field EV, while the operator specification defines the full field port.

## Precision

Python float inputs denote their exact binary64 values. For Float32 inputs, supply an already rounded binary32 value widened to Python float. `dtype=32` selects binary32 output and does not cast all inputs. Explicit static coefficients use their declared binary64 values; exact model constants and published decimal tables use fractions where the specification identifies them as mathematical constants.

`reference/precision.py` evaluates rational formulas with `Fraction`. Direct conversion uses nearest/ties-to-even, including subnormals, signed underflow and infinity on overflow. Transcendentals use directed MPFR interval bounds. A result is returned only when both enclosing endpoints round to the same destination bit pattern. Comparisons require interval separation or an established equality. `Uncertified` reports an unresolved rounding or comparison boundary; high precision alone is not treated as a proof. Reference-domain diagnostics are independent of the production unchecked contract.

Complete member expressions round at their output. Named intermediate feature, model, table and parameter outputs round to Float64 at the specification's boundaries. The Floyd-Steinberg recurrence strictly rounds each declared Float64 state operation, rather than retaining unbounded rational diffusion state. Stored palette RGB entries are copied after selection. Explicit copy paths preserve source bits within the mathematical finite-input API.

Compare native continuous results using the GRD tolerances and record absolute/relative errors. Compare copies, integer outputs and palette membership exactly. Report discrete decisions near rounding-sensitive thresholds as discrete mismatches; a small color error alone does not establish the same mask or dither pattern. The reference check command verifies the oracle, not conformance of any production implementation.

## Reference modules

| Module | Mathematical coverage |
| --- | --- |
| `reference/precision.py` | Exact fractions, direct IEEE rounding, certified MPFR enclosures |
| `reference/point.py` | Exposure, affine/LGG, levels, native model edits, tonal weights, saturation, selectors, CDL |
| `reference/curves.py` | Linear/PCHIP interpolation, ramps, LUTs and displacement meshes |
| `reference/statistics.py` | Binary/weighted quantiles, histograms, CDF, CLAHE and parameter states |
| `reference/spatial.py` | Local thresholds, bilateral log base and vignette geometry |
| `reference/dither.py` | Ordered/noise quantization, Float64-state diffusion, dot/line screens |
| `reference/color.py` | RGB matrices, white adaptation, Lab/OKLab, four brightness routes and temperature/tint |
| `reference/extended.py` | Complete image functions, neutral selection, palette dither and artistic screen composition |

`color.Context` takes an explicit RGB-to-XYZ matrix and reference white. Its default sRGB/D65 basis is a fixture default; operation specifications still require explicit source interpretation. Coordinates in built-in primary/white presets are binary64 values; model matrix coefficients follow the FMT mathematical definitions. `temperature_white` uses the bundled, unchanged [CIE observer dataset](data/README.md), with strictly rounded Float64 white as the adaptation input. The source temperature means the illuminant being corrected.

## Coverage limits

One representative sample per member does not cover its entire domain. The references define numerical functions, not operation registration, tensor layout, selected-channel assembly, alpha/AOV payload transport, memory ownership, cancellation or resource behavior. Complete RGB wrappers accept selected RGB triples; image-alpha statistical weighting is an explicit argument. Runtime ROI/tile support must be checked against a future implementation at identical global coordinates; spatial oracle functions compute the full canonical small image or a selected mathematical observation.

The references neither execute production code nor provide commercial software compatibility evidence. A native implementation may use different arithmetic staging within its allowed error, while the strict mathematical value remains the comparison target.
