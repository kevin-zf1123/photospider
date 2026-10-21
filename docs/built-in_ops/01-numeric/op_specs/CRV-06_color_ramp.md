---
spec_schema_version: 1
id: CRV-06
kind: shared_operator_contract
category: 01-numeric
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
---

# CRV-06: color-ramp family

## Native lightness and explicit description

The [shared scale contract](../../02-format-color/op_specs/FMT_relative_coordinate_scale.md)
uses native CIELAB/CIELCh `l=L*/100` for ramp colors, LUT input axes, output
table coordinates and validation points. Finite values outside 0..1 remain
legal. Opponent/chroma scales, hue winding and arithmetic formulas are unchanged.
The implementation uses canonical TensorDescription v4/v5 tuple parameters.
The old ColorArray types and decoder are removed; old payloads and explicit
L* units are rejected without sample-magnitude inference.

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

Each dynamic port is a Result containing exactly one tensor member.
Any structurally valid schema id/version/member key is accepted. Validate shapes
against each member's complete `sample_shape()`, including batch axes. The ports
are `input` (rank 1..7, Float32/64), `stops` (rank 1, K=1..65536, Float32/64),
`colors` (rank 2, [K,C], Float32/64), and for RationalPi variants
`hue_numerator` and `hue_denominator` (rank 1, [K], Int64). RationalPi colors
use [K,2] model-specific component arrays; all other colors use [K,C]. The
first three floating inputs independently select their dtype. All input, color
and output logical element products are limited to 2^40. Only a mathematically
selected RationalPi denominator q must be positive; an unselected generic
q<=0 value adds no numerical precondition. Upstream failures propagate through all declared source tiles.

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
precision, dependencies and output semantics. The shared TensorDescription tuple
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

Color ramp configuration is supplied by the required String `color_description`,
encoded with `tensor_description_parameter`. It describes one logical tuple
with `channel_axis=0`; the operation separately validates its physical port shape
and member model. Dtype and out_of_domain are explicit parameters; polar members
also declare output_hue_unit. RGB input/output is straight, and the removed
output_association parameter is rejected. No Result carries a color description.

The standard sRGB transfer is independently documented in
[W3C CSS Color 4](https://www.w3.org/TR/2026/CRD-css-color-4-20260913/#valdef-color-srgb).
This is primary reference evidence, not adoption of CSS's complete interpolation
or rendering behavior. The project's [color-management category](../../02-format-color/representation.md)
already separates assignment, transfer, RGB matrix and model conversion.

The maintainer selected a new
[generic color-array description](../../02-format-color/op_specs/FMT-COLOR_color_array_contract.md)
supporting Float32/Float64 and arbitrary legal shapes with a final color axis.
Callers provide the required color configuration to every downstream consumer;
the output itself carries only structure and numerical samples.

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

## Fixed execution and caller preconditions

Static preparation validates complete input schemas, port arity, K, shape products, dtypes and color descriptions. `values` uses a fresh `photospider.tensor` v1 schema, member `samples`, descriptor shape `sample_shape(input)+[C]` and `atomic_trailing_axes=1`. It carries no color description and does not preserve the input's batch-axis partition. The output channel axis stays complete in each selected tile.

Each selected output tile lowers to one CPU `color-ramp` / `color-ramp-fixed-v2` task using invocation ABI 8. Its query region is the output region without the component axis. Stops, colors and optional p/q arrays have conservative complete closures because row selection depends on query samples. Every region closes on its own source grid; physical grants may contain unused samples. Empty emits no tasks or sample reads. K=1 still reads the requested query positions and selected stop/color row.

The caller guarantees all stops are finite and strictly increasing, consumed queries are finite, and reject-policy queries lie in the closed stop domain. Selected complete colors must be finite and obey their model and association rules; selected rational denominators must be positive, including at zero chroma/saturation. Unselected generic color rows and p/q pairs carry no consumer numerical precondition. Final components must be finite in the output dtype. For RGBA, a positive exact alpha that rounds to zero requires all rounded RGB components to be zero; this valid transparent tuple canonicalizes to +0. Numerical violations have no promised runtime domain or overflow diagnostic.

The kernel locates a selected row or adjacent pair by binary search in the source stops. Direct selection and complete stored-row identity retain the model's direct conversion rules. For rational hue, identity compares raw p/q as well as both floating components; equivalent unreduced fractions alone do not imply row identity. RGB interpolation evaluates the full transfer/association expression. Other models interpolate their coordinates, with certified pi multiplication/division for cross-unit hues. There is no modulo reduction, shortest-arc selection or achromatic hue substitution.

Preparation checks exact output metadata, canonical regions, source slot order and completeness, dtypes, Host placement, encoded parameters and scratch bounds. Each invocation borrows immutable source views and owns its mutable arithmetic scratch. Offsets, unaligned addresses and negative/zero source strides are supported within host-validated bounds. The kernel allocates no heap storage and writes only preallocated selected output tiles. The executor publishes successful call Results after all required tasks succeed in retirement order. Output backing survives context teardown.

Lookup costs O(P log K) for P query points in a tile, plus exact/certified arithmetic. Stops are searched directly without copying an O(K) index. Scratch contains one ExactRgb or ExactColorCoordinate instance, independent of P and K; complete current color packets and coordinates have fixed bounds. Source-tile records, retained source owners, input COPY tasks and output allocations have separate costs. Lowering bounds read-tile counts before enumeration. Cancellation is polled in lookup and arithmetic. Checked arithmetic separates retryable precision uncertainty from terminal capacity/cancellation failures, and each refinement checks status before accepting results. The floating environment is restored at return.

Malformed static parameters/descriptions fail with InvalidArgument; incompatible shapes, dtypes and schemas use TypeMismatch. Resource admission, unavailable profiles, stale-plan, upstream failures and cancellation retain operational status. Incomplete certification returns ResourceExhausted and never selects an approximate substitute.

## CMYK resources

CMYK declares the profile's content identity in `OperationPreparation.resources`. `OperationDefinition.validate_resources` inspects the selected frozen bytes once per PreparedOperation, including Empty calls and artifact import. It requires a valid ICC v2/v4 `prtr`/`CMYK` profile, validates header/tag structure and optional v4 Profile ID, and has a 64 MiB static work limit. It borrows frozen bytes without a full-profile copy or filesystem I/O. Changing profile identity requires recompilation.

Preparation, Plan and active kernel owners retain the selected resource. Color Results retain their output data; they acquire no profile owner through a color description. Interpolation does not execute ICC transforms or infer total-ink limits.

## Maintained implementation and validation

[`color_ramps.cpp`](../../../../plugins/ops/01-numeric/color_ramps.cpp) registers 45 profile keys for 15 independent entrypoints. [`color_ramps.hpp`](../../../../plugins/ops/include/photospider/ops/numeric/color_ramps.hpp) supplies their public constructors. All profiles currently use exact/certified whole-expression arithmetic. RGB uses a 40960-bit polynomial arena; coordinates use 9216-bit integers. Directed work has a 12288-bit arena and a 128..4096 fractional precision ladder. Checked state and borrowed WorkConsumer callbacks propagate cancellation and capacity without exception-based control flow.

Temporary acceptance uses the independent [coordinate oracle](../../../../oracle/ops/numeric/crv06/reference.py), [RGB oracle](../../../../oracle/ops/numeric/crv06/rgb.py) and [ICC fixtures](../../../../oracle/ops/numeric/crv06/icc.py). Cover valid numerical cases, explicit raw negative zeros, all model/unit/dtype combinations, straight RGB alpha, sparse tiles, mixed source grids, concurrent Runs, direct affine layouts, cancellation, allocation counts, scratch/output boundaries, resource admission and lifetime. Static malformed ICC cases remain rejection tests; invalid numerical preconditions do not define runtime failure tests. Remove temporary operator tests after validation and retain the independent oracles.

CIELAB/CIELCh uses native `l=L*/100` throughout the migrated public API,
independent fixtures and consumers. [Migration evidence](../NUM-CRV_color_migration.md)
records executed profiles. Registration alone does not establish physical x86
or GPU execution. This family has CPU implementations and remains Proposed.
