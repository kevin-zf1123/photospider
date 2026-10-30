# Strict basic compositing and keying references

These independent mathematical references accompany the [CMP/BLN/NBL catalog](../../../docs/built-in_ops/09-composite/blending.md) and [KEY catalog](../../../docs/built-in_ops/09-composite/keying.md). They cover 92 proposed basic tile members. PNT painting, repair, patch and OpenCV profiles are outside this reference package.

## Run

From the repository root:

```sh
python3 oracle/ops/09-composite/run.py --check
python3 oracle/ops/09-composite/run.py --check --dtype f32
python3 oracle/ops/09-composite/run.py --member KEY-05A
python3 oracle/ops/09-composite/run.py
```

Python's standard library is sufficient. The commands perform no downloads and write no reports. `--check` checks the oracle's authored sample values and additional mathematical properties. Ordinary output is JSON lines of strict member values. The file contains one representative sample per member, not exhaustive domain coverage.

`catalog.REFERENCES` maps member IDs to reusable Python callables; `catalog.SAMPLES` provides arguments for the representative observations. Linear and encoded coverage variants share algebra, so the caller supplies values already in the chosen domain. The oracle never inserts a transfer conversion. A color-property or whole-color native-model selector receives values already converted to its declared model.

## Strict arithmetic

The fixture adapters round float inputs to their requested `f32` or `f64` storage before evaluating the reference. T scalar controls use that same dtype. Working-basis coefficients are stored Float64 values and are normalized as specified. Fractions inside equations denote exact quantities; printed mathematical constants and integer weights are exact references.

Algebra uses `fractions.Fraction`; final rational-to-IEEE conversion implements nearest/ties-to-even directly, including subnormal, signed-underflow and infinity results. Square-root expressions are enclosed by exact rational bounds derived from integer square roots. A radical result is returned only when both bounds round to the same destination bit pattern; otherwise the reference reports `UncertifiedReference` after its refinement limit. No high-precision approximation is silently accepted as a strict answer.

Named distance and Y-measure outputs round at their declared stage boundaries. Other scalar equations are evaluated as complete mathematical formulas before final rounding. Production operators use native FP32/FP64 and the comparison criteria in [COM](../../../docs/built-in_ops/09-composite/op_specs/COM_common_contract.md). Reference domain exceptions are tool diagnostics, not production payload-validation requirements or public status enums.

## Modules and callable representation

| Module | Responsibility |
| --- | --- |
| `reference/exact.py` | Exact fractions, direct IEEE rounding, certified affine radical results and small rational solves |
| `reference/blend.py` | Coverage, color functions, three whole-color measures, seeded dissolve, image interpolation and native/W3C property exchange |
| `reference/keying.py` | Range/color keys, background evidence, matte refinement, foreground/spill estimates, edge extension and matting |
| `catalog.py` | Member-to-callable mapping and authored samples |
| `run.py` | JSON reference output and oracle self-checks |

Pixel functions use GrayA/RGBA lists or color-only triples as small fixture adapters. They do not implement the production arbitrary-channel Result interface, AOV assembly or Region requests. Image interpolation receives one t value for its selected pixel observation; the operator's full port is a matching spatial field. Color-only foreground estimation returns RGB, with alpha kept as a separate value for explicit downstream assembly. The mathematical adapters expect finite active samples; bit-level NaN transport through arbitrary runtime layouts requires separate runtime checks.

## Matting reference

The small-grid matting oracle assembles the KEY clipped-window Laplacian from actual stored samples, enforces trimap constraints and solves the rational unknown system by independent elimination. Its raw residual is exactly zero, while clamping may alter the final matte's equation residual. `catalog` exposes `matte` and `raw_residual`; the lower-level function also returns exact solution/residual diagnostics for investigation.

The dense reference is limited to 64 total matting pixels and 64 unknowns for a generic solve. Those are oracle safety limits, not production extent constraints. Missing evidence and singular systems produce oracle diagnostics. Production convergence and operational failure behavior are defined in the KEY mathematics, independently of this exact reference's algorithm.

## Comparison scope

Use strict values to inspect native absolute/relative error, with exact checks for prescribed copies, endpoints and selected tuples. Check a discrete decision mismatch separately from continuous color error. The seeded dissolve reference uses global coordinates; re-evaluating a pixel with the same seed/frame must retain the same branch.

These self-checks establish properties of the reference program. They do not establish operation registration, runtime tile dependencies, arbitrary tensor layout, input lifetime, cancellation, resource accounting or commercial software conformance. A future implementation must compare actual tile/ROI results at identical coordinates and exercise independent output requests separately.
