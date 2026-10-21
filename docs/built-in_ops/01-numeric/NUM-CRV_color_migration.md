# NUM/CRV color-description migration

Completed in the `photospider` workspace on 2026-10-21. Specification decision
status remains **Proposed**. This record covers the independent NUM/CRV migration
after the active FMT implementation work.

## Outcome and frozen boundary

CRV-06, CRV-07 and CRV-09 now consume the existing operations-SDK
`TensorDescription` v4/v5 encoding. Native CIELAB/CIELCh lightness is
`l=L*/100` throughout authoring, ramp inputs/outputs, LUT axes/tables, bake
sampling and reports. RGB ramps consume and emit straight RGB/RGBA.

The frozen baseline is `8d915456f2cc204bce3d690d8304ba80718cd69e`.
There are no modifications under `src/lib/**` or the kernel public
`include/photospider/**` outside `include/photospider/ops/**`.
Workflow serialization, IR, planner, executor, kernel invocation ABI 8 and
generic compiler artifact containers are unchanged. Changes are confined to
the operations SDK, numeric plugins, their build sources, independent oracles
and related documentation. Existing FMT work is preserved.

The retired public header, ColorArray codec/geometry implementation, unused
typed sample validator and unused LUT table-scanning helper are removed. No
forwarding header, type alias, old overload or legacy payload decoder remains.
The rebuilt SDK archive exports no ColorArray or retired table-validation
symbols. The old oracle filename is replaced by `common/color_geometry.py`.

## Public interface and semantics

| Area | Current contract |
| --- | --- |
| Color configuration | `TensorDescription`; existing `tensor_description_parameter` and byte codecs |
| Numeric tuple helpers | `ps::numeric::color_tuple_description`, `resolve_color_tuple`, `validate_color_tuple_shape` |
| Hue unit | `ps::numeric::HueUnit`: Radian, PiMultiple, RationalPi |
| RGB authoring | `ColorRampOptions`; `color_ramp_rgb_description(bool with_alpha=false)` |
| CMYK authoring | `color_ramp_cmyk_description(ResourceIdentity)` returns `Result<TensorDescription>` |
| Removed RGB option | `RgbRampOptions` and `output_association` |
| Bake configuration | Both complete description encodings enter `photospider.bake-configuration.v2` |
| Owned bake table/report | `curve.bake_lut3d.table` / `curve.bake_lut3d.report` schema v4 |
| Exported ordinary table | Unchanged `photospider.tensor` v1, member `samples` |
| Fixed operation contracts | `color-ramp-fixed-v2`, `lut3d-fixed-v2`, `bake-geometry-fixed-v2`, `bake-result-unchecked-v3` |

The report still contains thirteen static tensor members and 361 payload bytes.
`measured_table_id` still binds the actual owned Result ObjectId. Gate order is
configuration, table identity, then measured tolerance. Old v3 table/report
schemas are rejected. Schema versions, description codec versions and the hash
domain version are distinct.

A numeric description represents one logical tuple of shape `[C]` with
`channel_axis=0`, one complete canonical model group and optional independent
straight alpha at its final index. It does not describe leading sample
dimensions. Actual ports remain `[K,C]`, `[...,3]`, `[N0,N1,N2,3]` or the
member's explicit split-hue ports. Group, global and channel assertions must
agree. Group components cannot contain independent interpretation overrides.

The resolver rejects numeric encoding/sampling, unrelated components,
absolute coordinates, configured-space/analytic bindings and external
premultiplied values. Native models use `relative-v1` and scene/display
reference. CMYK uses `icc-native` and profile reference; its printing ICC
resource still undergoes the existing static admission.

CRV-06 permits alpha only in its RGB member. CRV-07/09 require same-model,
three-component input/output tuples with no alpha and floating hue. Split
rational hue belongs to its three ramp families and retains exact Int64 p/q
until the specified final conversion. Output Results contain no descriptions.

For composition with FMT, explicitly author the actual tensor's channel axis.
A numeric tuple can use axis zero while the resulting `[N,3]` tensor needs
`channel_axis=1` in the FMT input description. Coordinate meaning is unchanged.

### Minimal normalized Lab authoring

```cpp
ps::TensorInterpretation lab;
lab.model = "cielab";
lab.reference = "display";
lab.association = "straight";
lab.white = std::array<double, 2>{.3457, .3585};
lab.coordinates = ps::TensorModelCoordinates{};
lab.coordinates->scale = "relative";

auto tuple = ps::numeric::color_tuple_description(lab, {"1", "a*", "b*"});
if (!tuple.ok())
  return tuple.status();

// Pass tuple.value() as the explicit description to a color-ramp/LUT helper.
// colors such as [0.5, 20, -10] mean L*=50, a*=20, b*=-10.
auto fmt_description = tuple.value();
fmt_description.channel_axis = 1;  // Explicit FMT interpretation of [N,3].
```

Old L* sample data must be explicitly converted by the caller; old workflows
must be reauthored and recompiled. Numeric value 50 can be valid HDR normalized
lightness and is not rejected by magnitude inference. CIELAB a*,b*,C*, OKLab
coordinates, hue turns and CMYK ink scales do not change.

CRV-09's single scalar `atol` applies to all three stored components. Dividing
it by 100 would also tighten unchanged opponent/chroma thresholds; reselect
tolerances in the intended new units and regenerate the bake/report. Complete
description bytes enter the configuration digest, so semantically equivalent
descriptions with different explicit records need not have the same identity.

## Implementation and performance decisions

The tuple resolver reuses the existing TensorDescription structural checker,
FMT RGB basis/white admission and transfer codec. It does not modify the shared
codec or add runtime numeric scans. Exact geometry checks run with the required
floating environment and restore caller state.

RGB preparation resolves transfer kind, gamma bits and alpha presence once.
`ExactRgb` removes premultiplied storage decode/output branches and the exact
ratios that served them. It preserves internal alpha-weighted linear-light
interpolation, transparent-black canonicalization, direct/identical-row zero
signs, exact gamma=2 root rounding and certified nonlinear evaluation. Coordinate
and LUT interpolation retain their existing exact arithmetic. Existing
scalar/NEON/AVX2 integer paths remain; no lower-precision float approximation
or new SIMD arithmetic is introduced.

### Comparable kernel measurements

The repository `performance` skill guided a temporary before/after kernel
comparison and a separate macOS sampling run. Baseline `ExactRgb` came from the
frozen baseline revision; it was compiled only in a temporary benchmark with
its straight-input/straight-output path selected. No old interface is retained
in production.

Environment: Apple M5, ARM64, macOS 27.2 (26B5101f), AppleClang 21,
C++17 `-O2 -g`, repository RelWithDebInfo libraries. The benchmark reused one
arithmetic arena per implementation, excluded construction and compared the
same deterministic sixteen-color dataset with mixed RGB/RGBA, Float32/Float64
outputs, signed/HDR values and small alpha. Before/after output bits matched
in all 160 distinct profile/path/color comparisons.

Each row below is the median of seven runs with alternating before/after
measurement order. Times are milliseconds for the listed color count.
These are arithmetic microbenchmarks, not end-to-end workflow throughput.

| Profile | Path | Colors/run | Before ms | After ms |
| --- | --- | ---: | ---: | ---: |
| strict | direct | 512 | 1.50346 | 1.49412 |
| strict | linear | 512 | 32.4757 | 32.4950 |
| strict | gamma=2 | 512 | 57.5950 | 57.1464 |
| strict | gamma=2.2 | 32 | 47.1316 | 46.4366 |
| Apple Silicon | direct | 512 | 1.59250 | 1.55871 |
| Apple Silicon | linear | 512 | 31.6765 | 30.2465 |
| Apple Silicon | gamma=2 | 512 | 58.8284 | 57.9260 |
| Apple Silicon | gamma=2.2 | 32 | 43.7890 | 43.5171 |
| strict | sRGB, longer confirmation | 512 | 776.696 | 778.687 |
| Apple Silicon | sRGB, longer confirmation | 512 | 743.766 | 748.110 |

The initial 32-color sRGB samples suggested +5.60%/+3.26% median time, so only
that concrete uncertainty was rechecked with longer runs. The 512-color
confirmation measured +0.26%/+0.58%, with overlapping ranges:
strict before 773.118..783.748 ms versus after 773.012..797.453 ms; Apple before
738.768..766.263 ms versus after 739.498..769.161 ms. The larger apparent
regression did not reproduce. These results support performance parity for
sRGB at this resolution, not a general speedup claim.

A separate `/usr/bin/sample` run sampled the new Apple Silicon path for five
seconds at a requested 1 ms interval. It recorded 3,758 main-thread samples;
3,502 included nonlinear evaluation. Collapsed top frames included interval
integer division (879), addition (507), memory moves (413), memory clearing
(319) and interval multiplication (296). Production frames were symbolized;
the tool did not provide a separate lost-sample statistic. This identifies
certified arithmetic as the dominant work for this mixed workload. Sampling
duration was not used as benchmark timing. It does not justify changing
correct-rounding boundaries or the frozen kernel.

## Executed numerical and workflow validation

Temporary adapters invoked the actual public authoring helpers, Compiler and
ExecutionContext with the independent Python references. Every valid numerical
case ran under both strict and the Apple Silicon profile.

| Family | Cases per profile | Total executions |
| --- | ---: | ---: |
| Coordinate/rational-hue ramps | 1,505 | 3,010 |
| Straight RGB/RGBA ramps | 363 | 726 |
| Trilinear/tetrahedral LUT3D | 924 | 1,848 |
| Complete LUT3D bakes | 384 | 768 |
| **Total** | **3,176** | **6,352** |

Ramp and LUT samples matched the independent expected IEEE words exactly.
The 768 bakes used identity and square source graphs, both table interpolation
methods and explicit source/table Float32/Float64 choices. They produced
418 passing and 350 intentionally failing quality gates. Every report's
counts, exact-upward error bits, maximum indices/points and first-failure
coordinates matched the independent Fraction oracle. Failure checks required
the tolerance error category/reason/message, and the retirement observer
retained the successful report after a later gate failed.

The adapters exercised unaligned negative-stride source views in every
eleventh case. The temporary planner explicitly declared those source layouts,
and each DirectTiles input tile had its own backing. An initial fixture
incorrectly declared DenseHost/shared backing; it was corrected in the
temporary adapter without any kernel change.

Additional acceptance:

| Check | Passed cases/checks |
| --- | ---: |
| Independent exact white/primary geometry admission | 1,847 |
| Valid tuple/codec/shape assertions | 16 |
| Invalid tuple/codec/shape assertions | 26 |
| Removed/static ramp workflow parameters | 5 |
| Empty selection still rejects static/resource errors | 6 |
| Legacy description endpoints and old Gate schemas | 17 |
| Actual fully populated v3 report rejected by public reader | 1 |
| Bake schema/configuration digest/reader checks | 120 |
| Multidimensional query shape preserving logical tuple meaning | 1 |
| Actual normalized Lab ramp followed by FMT Lab-to-XYZ | 1 |

Static checks cover old ColorArray bytes, explicit L*/percent units, conflicting
global/channel metadata, wrong axes/order, encoding/sampling, alpha mistakes and
split rational hue misuse. The removed `output_association` parameter was tested
on the actual RGB member. Both input and output description changes alter the
bake digest and cause report-reader rejection. Old descriptors were injected
into each affected parameter position in an authored bake graph, including its
LUT application and result stages.

The explicit CRV-to-FMT chain interpolated Lab l=0..1 at query 0.5, then converted
that output with physical channel axis 1. Expected XYZ bits came independently
from `Y=(33/58)^3` and exact D50 xy ratios, demonstrating the normalized l meaning
through an actual consumer.

The independent mathematical references also contain domain/overflow cases.
The adapter omitted cases outside fixed NUM caller premises rather than treating
them as promised runtime diagnostics: 287 coordinate-ramp, 5 RGB, 142 LUT and
24 selected bake fixture variants. No general sample validator is added.

## Build, hygiene and reproducibility

Executed gates:

- `cmake --build build/ops-dev --target photospider_ops -j 6`: passed.
- clang-format 21.1.3 and cpplint 2.0.2 on all fifteen touched C++ files: passed.
- `git diff --check`: passed.
- Frozen kernel/public-header path comparison against the baseline: no changes.
- Production/source/build-list scan for retired API/type names: no matches.
- Rebuilt operations-SDK archive symbol scan: no retired exports.

Temporary C++ workflow/benchmark adapters, baseline header, native binaries,
fixture generator and generated payload files are removed after validation.
Retained independent references are in
[the numeric oracle directory](../../../oracle/ops/numeric/README.md).
Build-local logs retain the run summaries, build/lint output, timing CSVs and
separate sampling output. No runtime test suite or benchmark executable is
added to the repository.

The executed host was ARM64. x86 profile registration is preserved but no
physical x86 execution is claimed. No GPU color-ramp/LUT behavior, broad CTest
pass, sanitizer pass or continuous-domain LUT error bound is inferred from
these focused checks.

## Related contracts

- [Shared explicit color/tuple contract](../02-format-color/op_specs/FMT-COLOR_color_array_contract.md).
- [Native relative units](../02-format-color/op_specs/FMT_relative_coordinate_scale.md).
- [CRV-06](op_specs/CRV-06_color_ramp.md), [CRV-07](op_specs/CRV-07_apply_lut3d.md),
  [CRV-09](op_specs/CRV-09_bake_lut3d.md).
- [Numeric implementation](math-implementation.md).
- [Tensor description architecture](../../kernel-architecture/Tensor-Semantic-Metadata.md).
