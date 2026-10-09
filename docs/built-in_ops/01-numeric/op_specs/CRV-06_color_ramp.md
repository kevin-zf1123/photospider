---
spec_schema_version: 1
id: CRV-06
kind: shared_operator_contract
category: 01-numeric
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_subset
clarification_status: complete
---

# CRV-06: color-ramp family

## Revised lightness coordinate and implementation boundary

The [shared scale contract](../../02-format-color/op_specs/FMT_relative_coordinate_scale.md)
requires native CIELAB/CIELCh l=L*/100, including ramp stops' color values,
LUT input axes and output table coordinates. Finite values outside 0..1 remain
legal. Opponent/chroma scales and arithmetic formulas are unchanged. The current runtime ColorArray v1 encodes L* in its implicit 0..100 unit;
this target scale applies only after public metadata, fixtures and consumers
adopt it explicitly. No implicit unit alias or inference from sample magnitude
is permitted.

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

## Confirmed representation and model split

Use explicit dynamic stops[K] and colors[K,C] rather than only a uniform LUT.
Each scalar element of input maps to one complete color, with output shape
sample_shape(input)+[C]. Nonuniform stop spacing is supported. This scalar-to-color
mapping differs from CRV-05's independent per-input-channel table application.

## Result tensor ports

Each dynamic port is a Result containing exactly one tensor member and no fields.
Any structurally valid schema id/version/member key is accepted. Validate shapes
against each member's complete `sample_shape()`, including batch axes. The ports
are `input` (rank 1..7, Float32/64), `stops` (rank 1, K=1..65536, Float32/64),
`colors` (rank 2, [K,C], Float32/64), and for RationalPi variants
`hue_numerator` and `hue_denominator` (rank 1, [K], Int64). RationalPi colors
use [K,2] model-specific component arrays; all other colors use [K,C]. The
first three floating inputs independently select their dtype. All input, color
and output logical element products are limited to 2^40. Only a mathematically
selected RationalPi denominator q must be positive; an unselected generic
q<=0 value adds no mathematical-domain failure. Typed/upstream validation still
covers every input tensor.

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
sample_shape(input)+[C] output and configurable RGB encoding are not already supported
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
through `photospider/ops/numeric/color_ramps.hpp`; this shared contract is not itself
a registered operation. ColorArray codec/metadata and full-color closure are
implemented, with ICC import/bind and immutable ownership. Result output schemas select the
resource identities required by their ColorArray facets.

RGB direct, linear and gamma=2 paths are exact; sRGB and general-gamma paths use
certified full expressions. Gamma normalization and deferred scaling preserve HDR
and alpha precision. Coordinate arithmetic uses up to 9,216 bits; RGB polynomial
work uses 40,960 bits. The 4,096-bit value is the interval precision ceiling for
relevant certified steps, not the integer-capacity bound. Accelerated RGB permits
the shared FP32 4 ULP by contract. Current implementations return strict bits,
while the same accelerated allowance applies to non-RGB arithmetic.

Its
`color_ramp_workflows`, `color_ramp_boundaries`, `color_ramp_icc` and
`color_ramp_active_cancel` coverage includes Empty ICC behavior, generic clamp
resource selection, ICC ownership, and cancellation during actual exact work
(RGB's 640-limb slot and the coordinate path's 144-limb slot). Those active
cancellation paths return Root resources to baseline after context teardown.
Successful ICC and Result-lifetime cases retain their valid owners.

 Against the Result `--probe`, `color_ramp_oracle.py` passes 1,784
Fraction/Machin-pi cases per profile and `rgb_ramp_oracle.py` passes 352
rational/root/Decimal cases per profile.
Repeated requests return consistent values and rebinding changes associations;
these checks do not establish a warm-cache hit.

Reproduce the focused suite, Strict/Apple manual groups and independent oracles
with:

```sh
cmake --build build/kernel-dev --target test_numeric_result_math_color_ramps photospider_numeric_color_ramps -j8
ctest --test-dir build/kernel-dev -R '^test_numeric_result_math_color_ramps$' --output-on-failure
build/kernel-dev/examples/numeric_workflow/photospider_numeric_color_ramps strict
python3 oracle/ops/numeric/color_ramp_oracle.py build/kernel-dev/examples/numeric_workflow/photospider_numeric_color_ramps strict
python3 oracle/ops/numeric/rgb_ramp_oracle.py build/kernel-dev/examples/numeric_workflow/photospider_numeric_color_ramps strict
build/kernel-dev/examples/numeric_workflow/photospider_numeric_color_ramps apple
python3 oracle/ops/numeric/color_ramp_oracle.py build/kernel-dev/examples/numeric_workflow/photospider_numeric_color_ramps apple
python3 oracle/ops/numeric/rgb_ramp_oracle.py build/kernel-dev/examples/numeric_workflow/photospider_numeric_color_ramps apple
```

Reproduce the installed consumer check with:

```sh
cmake --install build/kernel-dev --prefix build/kernel-dev/consumer-install
cmake -S tests/consumer -B build/kernel-dev/consumer-build -DCMAKE_PREFIX_PATH="$PWD/build/kernel-dev/consumer-install"
cmake --build build/kernel-dev/consumer-build --target photospider_result_numeric_consumer -j8
ctest --test-dir build/kernel-dev/consumer-build -R '^installed_result_numeric$' --output-on-failure
```

These checks cover Strict and the local Apple profile only. No x86 numerical,
maximum-shape, or performance rerun is included. CRV-06 has no GPU variant.
The ColorArray v1 CIELAB/CIELCh implicit L* scale difference from the native
`l=L*/100` contract remains open. See the [workflow README](../../../../examples/numeric_workflow/README.md#color-ramps)
and [math implementation](../math-implementation.md#crv-06-colorarray-and-color-ramps)
for the implementation and evidence boundaries.
## Whole execution and resources

All formal strict and accelerated keys use Whole Result programs. A nonempty
Run declares Data, Validation and Descriptor needs (role 13) for every input, so
typed and upstream validation covers complete tensor members. The executor reads
authorized windows directly; the callback does not collect or copy complete input
arrays. It validates all stops, then every input position, before color arithmetic.
Each position uses one exact hit/clamp/singleton row or two enclosing rows for its
mathematics. Generic color rows outside all evaluated stencils remain numerically
unused, while their typed/upstream validation still applies. Empty reads no sample
payload after static preparation.

The `values` port publishes an immutable `photospider.tensor` v1 Result with
`samples` shape `sample_shape(input)+[C]`, the ColorArray v1 facet and
`atomic_trailing_axes=1`. Whole writes publish the full output transactionally;
the Result retains full certified coverage and global coordinates. Fragments expose
requested regions while retaining the full output owner. Arbitrary immutable source
strides, offsets and unaligned storage are supported. Result association records
source ObjectIds, and selected ColorArray resources remain owned by the output.
Input edits invalidate complete recorded output demand. Numeric errors have Run
scope; failed callbacks publish no color subset. Upstream, resource and cancellation
errors retain their categories.

Lookup work remains O(K+N log K), plus exact or certified arithmetic. Full output
payload is N*C*sizeof(dtype), even for a small request. Account source owners/windows,
fixed arithmetic workspace, the Root-owned ResourceVector stop index (8K element
bytes plus allocator/metadata overhead), output and retained descriptors/resources.
No per-output dependency records or point-state array is retained. Work/capacity and
cancellation checks apply during scans, lookup, arithmetic and before publication;
incomplete certification fails ResourceExhausted. No reduced-precision fallback is added.
