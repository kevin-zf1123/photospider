---
spec_schema_version: 1
id: CRV-06
kind: shared_operator_contract
category: 01-numeric
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_subset
implementation_branch: numeric-optimize
implementation_base_commit: eb0e90c8
implementation_updated: 2026-09-21
clarification_status: complete
repository_branch: ops-specs
repository_commit: 6617c78c
---

# CRV-06: color-ramp family

## Revised lightness coordinate and implementation boundary

The [2026-09-23 shared scale revision](../../02-format-color/op_specs/FMT_relative_coordinate_scale.md)
requires native CIELAB/CIELCh l=L*/100, including ramp stops' color values,
LUT input axes and output table coordinates. Finite values outside 0..1 remain
legal. Opponent/chroma scales and arithmetic formulas are unchanged. Runtime
ColorArray v1 still encodes the old implicit L* units: public metadata, fixtures
and consumers need explicit migration before this revised target is implemented.
Historical implementation/test evidence below does not establish that migration;
no silent old/new unit alias or sample-magnitude inference is permitted.

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

## Confirmed representation and model split

Use explicit dynamic stops[K] and colors[K,C] rather than only a uniform LUT.
Each scalar element of input maps to one complete color, with output shape
input.shape+[C]. Nonuniform stop spacing is supported. This scalar-to-color
mapping differs from CRV-05's independent per-input-channel table application.

The maintainer requires separate implementations/specifications for RGB, CMYK,
XYZ, CIELAB, CIELCh(ab), OKLab, OKLCh, HSL and YCbCr. They are implemented as
separate current runtime paths, not as an unspecified color-model mode of one
numeric interpolator. Each linked specification records its exact color
definition, channel domains, reference parameters and alpha handling. CIELAB/CIELCh(ab) and OKLab/OKLCh are
separate required pairs; their scales, reference whites and hue conventions
must not be interchanged.

CIELAB, CIELCh(ab), OKLab and OKLCh initially use three color components without
alpha; coverage is separate. RGB alone currently defines its explicit RGBA
association behavior, while CMYK is four ink channels without alpha.

The model-specific operation clarifications are complete in the linked specs.
Each declares stop constraints, input/output types, interpolation, boundaries,
precision, dependencies and output semantics. The shared ColorArray
representation and the 45-key implementation are available through the current
public runtime while this family remains Proposed. Validation is recorded below;
remaining category work is tracked in [implementation.md](../implementation.md).

## Confirmed RGB color description requirement

RGB ramps explicitly specify gamut/primaries, white point and transfer function.
The maintainer selected default sRGB primaries, D65 and the standard piecewise
sRGB transfer function, correcting the earlier gamma-2.2 wording. RGB interpolation
is fixed in linear light, decoding input colors and encoding output colors in
the same specified space. Necessary color-management
dependencies may be clarified in their corresponding specifications, without
silently extending current accepted runtime metadata or conversion behavior.

Current typed Image validation in
[semantic.cpp](../../../../src/lib/data/semantic.cpp) accepts RGB only with
primaries=srgb and transfer=linear. Image samples are Float32 HWC; the generic
input.shape+[C] output and configurable RGB encoding are not already supported
typed Image metadata. The required color-description and transfer extensions
must be specified and implemented explicitly rather than relabeling incompatible
data as the existing profile.

The standard sRGB transfer is independently documented in
[W3C CSS Color 4](https://www.w3.org/TR/2026/CRD-css-color-4-20260913/#valdef-color-srgb).
This is primary reference evidence, not adoption of CSS's complete interpolation
or rendering behavior. The project's [color-management category](../../02-format-color/representation.md)
already separates assignment, transfer, RGB matrix and model conversion.

The maintainer selected a new
[generic color-array description](../../02-format-color/op_specs/FMT-COLOR_color_array_contract.md)
supporting Float32/Float64 and arbitrary legal shapes with a final color axis.
Color ramp outputs carry this complete color information to downstream consumers;
it is not silently discarded into an untyped numeric array.

- [Curve category](../curves.md).
- [RGB ramp specification](CRV-06A_color_ramp_rgb.md).
- [CMYK ramp specification](CRV-06B_color_ramp_cmyk.md).
- [CIELAB ramp specification](CRV-06C_color_ramp_cielab.md).
- [CIELCh entrypoints and hue contract](CRV-06D_color_ramp_cielch.md).
- [OKLab ramp specification](CRV-06E_color_ramp_oklab.md).
- [OKLCh entrypoints and hue contract](CRV-06F_color_ramp_oklch.md).
- [HSL entrypoints and contract](CRV-06G_color_ramp_hsl.md).
- [YCbCr ramp specification](CRV-06H_color_ramp_ycbcr.md).
- [XYZ ramp specification](CRV-06I_color_ramp_xyz.md).
- [Operator specification template](../../00-foundation/spec-template.md).
- [Foundation color and execution contracts](../../00-foundation/contracts.md).

## Maintained implementation and validation

The current runtime registers 45 CRV-06 keys and exposes 15 primitive helpers
through `photospider/numeric/color_ramps.hpp`; this shared contract is not itself
a registered operation. ColorArray codec/metadata and full-color closure are
implemented, with ICC import/bind and immutable ownership through compiler,
Value, snapshot and dependency paths.

RGB direct, linear and gamma=2 paths are exact; sRGB and general-gamma paths use
certified full expressions. Gamma normalization and deferred scaling preserve HDR
and alpha precision. Coordinate arithmetic uses up to 9,216 bits; RGB polynomial
work uses 40,960 bits. The 4,096-bit value is the interval precision ceiling for
relevant certified steps, not the integer-capacity bound. Accelerated RGB permits
the shared FP32 4 ULP by contract. Current implementations return strict bits,
while the same accelerated allowance applies to non-RGB arithmetic.

Native arm64 Clang 21 strict/Apple Whole validation covers seven public workflow
groups, 1784 independent Fraction/Machin-pi cases and 352 RGB rational-root/Decimal
cases per profile. Manual checks include complete-input query/typed failure,
Run-scoped association errors, sparse delivery, full dirty support, cache rebinding,
negative/unaligned and zero strides, rank-two traversal, fenv restoration, Empty,
work/output/workspace budgets, active cancellation and CMYK ICC ownership after
context teardown. Focused numeric, compiler, color and resource tests pass.
This Whole change has no new x86 or installed-package execution evidence.
Public commands and measurements are in the [workflow README](../../../../examples/numeric_workflow/README.md)
and [math implementation](../math-implementation.md#crv-06-colorarray-and-color-ramps).

## Whole execution and resources

All formal strict and accelerated keys use Whole execution. Any nonempty
request collects complete input, stops and color arrays (and both rational-hue
integer arrays when present), with complete upstream and typed validation.
The callback validates every stop, then every position, before color arithmetic.
Each position still uses exactly one hit/clamp/singleton row or two enclosing
rows mathematically. Unused generic color rows are not subjected to new numeric
domain checks; invalid typed data or upstream failures anywhere still fail.
Empty requests perform static preflight but read no sample payload.

The output is one immutable dense Value of shape input.shape+[C]. The final
channel axis retains complete-color closure, ColorArray identity and owned ICC
resources where applicable. Public fragments expose the requested complete
colors while retaining the full output owner. Arbitrary immutable input strides,
offsets and unaligned storage are supported. Owners survive context teardown.
Any input edit invalidates the complete recorded output demand. Cache identity
retains descriptors, parameters, typed validation and resource identities.
Numeric errors have Run scope; no successful color subset survives a failed
callback. Upstream, resource and cancellation errors retain their categories.

For N=product(input.shape), lookup work is O(K+N log K), plus actual exact or
certified arithmetic. Full output payload is N*C*sizeof(dtype), even for a small
requested region. Account complete collected inputs, fixed admitted arithmetic
workspace, a ResourceVector stop index with 8K element bytes plus allocator and
metadata overhead, and retained descriptors/resources. No per-output dependency
records or point-state array is retained. Work/capacity limits and cancellation
apply during scans, lookup, arithmetic and before publication; incomplete
certification fails ResourceExhausted. No reduced-precision fallback is added.
