# ADR 0020: Composable Numeric, Semantic, and Output Foundations

- Status: Accepted; locally implemented through #297, delivery review/merge pending
- Date: 2026-09-10
- Acceptance: the maintainer approved the G1/G2/G3/G5 implementation plan,
  including replacement of image v1 and migration of all eight existing ops.
- Baseline: `main@fba06270a44b24de7dcf12ac82624c4a56e0fa67`, package 0.6.0,
  operation ABI/OperationTraits 6
- Tracking: [#287](https://github.com/kevin-zf1123/photospider/issues/287)
- Reader mirror: [Chinese](zh/0020-composable-operation-foundations.zh.md)

## Decision and scope

Keep the existing Value/storage model, UInt8/Int64/Float64/Float32, C++17,
single `value` output per node, nonzero rank-1..8 shapes and immutable per-Run
bindings. Add reusable semantic descriptions, static output inference and
computed scalar composition to the compiler, direct registry invocation and
trusted pure C operation ABI through one shared implementation.

This decision supersedes ADR 0016's image-v1/nonnegative-RGB and direct-only
bounded-scalar clauses, and the package/operation/trait targets in ADR 0019.
ADR 0015 ownership, ADR 0017 storage/Region/budget and ADR 0018 snapshot/cache/
frozen ownership continue to apply. Existing regional image operations keep
their established Region rules. New shape-changing, LUT and global operations
start with Whole; no G4 per-port spatial dependency protocol is introduced.

G6 host assets, daemon migration, full paths, FFT, ICC/OCIO integration and the
remaining operation catalogue are outside this delivery. The research catalogue
under `built-in_ops` stays Proposed except for the subset fixed here. This ADR
accepts a target; it does not claim that the baseline registry exposes new APIs.

## G1: numeric rules

Float32 is the primary computation/storage type; coefficients, expression
evaluation, reference arithmetic and deterministic accumulators use Float64.
Generic Value retains every already-valid floating bit pattern. Finite/range
requirements belong to semantic descriptions and operations, not Value itself.

`numeric.cast` changes dtype without scaling. `numeric.encode_range` maps
`src_min < src_max` to `dst_min < dst_max`, then converts dtype. Both support
the four existing dtypes, use ties-to-even integer rounding by default and
reject overflow by default. `overflow="clip"` explicitly clips finite values;
NaN/infinity are rejected when integer output or range encoding is requested.
Float narrowing rejects finite overflow; floating cast preserves supported
non-finite values. Float-to-Int64 checks the rounded value before conversion,
without representing INT64_MAX as a rounded binary64 upper bound. Int64
identity/conversion must not unnecessarily pass through double.
Range evaluation uses Float64 affine arithmetic with a nearby endpoint anchor,
error-free high/low differences, compensated products/sums, explicit fused
multiply-add and nearest-even rounding. Unrepresentable affine coefficients or
unresolved quotient/accumulation error exceeding one output Float64 ULP fail
explicitly; finite source values whose evaluated result overflows the target
dtype obey reject/clip, including final floating overflow. Clip never admits
an originally non-finite source. Dither is fixed off in this delivery.

No implicit clamp, cast, broadcast, gamma operation or hidden epsilon is added.
Arithmetic accepts finite Float32/Float64 inputs of equal dtype and shape;
division by zero and non-finite computed results fail. Deterministic reductions
visit logical row-major samples in a fixed order using Float64 accumulation;
population variance uses a fixed two-pass mean/deviation algorithm (`ddof=0`).
CPU numerical tolerances are operation-specific; the initial finite reference
gate is `atol=1e-6, rtol=1e-5` unless an exact oracle is specified.

## G2: typed semantics and image v2

Add a public owned `SemanticDescriptor` and helpers to encode/decode and validate
it against a ValueDescriptor and regional samples. It describes a closed kind
(scalar, image, mask, scalar field, vector field, complex field, sampled signal,
LUT, or byte resource), ordered channel names/roles/units, color model,
primaries, reference white, transfer, scene/display reference, overall unit,
alpha association, coordinate space/direction, uniform sample origin/step and
axis unit.
Absent semantics remains a valid generic Value; it cannot satisfy a typed port
by shape coincidence. Mask roles distinguish coverage, probability and membership;
signed fields do not use the bounded mask kind. Complex fields use an explicit
real/imaginary component axis, with full unshifted spectrum/DC at index zero,
negative-sign unnormalized forward transform and inverse divided by N. The
closed tokens are `frequency_unshifted` and `forward_negative_inverse_1n` in
coordinate_space/direction. Vector coordinates are `pixel_displacement`,
`normalized_displacement`, `pixel_position` or `normalized_position`, with
forward/inverse direction. No complex dtype or FFT runtime is added.
Byte resources are UInt8 `[N]` with a bounded media-type label; no loading API.

Image semantics use the existing key `photospider.image`, version **2**.
Other typed kinds use `photospider.semantic`, version **1**. A typed Value has
at most one of these semantic facets. Image-v1 payloads, old profile constants,
port kinds and readers are removed from active interfaces; no adapter or alias
remains. Opaque unrelated generic facets retain the existing Value contract.

The canonical payload is an ordered binary record, independent of native
struct layout: kind; channel count; each channel's name, role and unit; model;
primaries; white X/Y/Z; transfer; reference; unit; association; coordinate
space; direction; sample origin/step; sample axis unit; media type. Text uses a
uint32 little-endian byte count followed by strict UTF-8 without NUL, numeric fields use IEEE-754
binary64 little-endian bits, and channel count is uint32 little-endian. Kind
and closed vocabulary are encoded by their lowercase documented strings.
Unused strings are empty and unused numeric fields are positive zero. Metadata
numeric fields must be finite; applicable white components are positive with
Y=1, and sampled step is positive. Semantic metadata zero is canonical positive
zero; sample bytes retain signed zeros. Bound typed payloads to 4096 bytes,
channels to 1..64 where present and each text field to 128 bytes. Reject unknown closed
tokens, inapplicable nonempty fields, duplicates, invalid combinations and
trailing bytes. These limits do not enlarge existing Value facet limits.
The public helper is the only producer of canonical payloads; C tables carry
copied pointer/count records of the same data with exact ABI validation.
Public typed helpers convert a descriptor to/from a static `semantic` String
parameter containing canonical lowercase hexadecimal payload bytes (at most
8192 characters). `color.assign` and `channel.merge` use this parameter;
workflow authors construct a typed descriptor and call the helper instead of
writing hexadecimal. This keeps WorkflowDocument schema 2 and the existing
String limit. Reject malformed/noncanonical hex, invalid semantic combinations
or oversized records with `InvalidArgument`, before callback/IR publication.
Generic opaque Value facet limits remain unchanged.

Image layout is Float32 `[H,W,C]`, ordered channels agreeing with C. The first
workspace is linear sRGB/Rec.709 RGB, D65, scene-referred relative units; finite
signed/HDR RGB is legal. RGB, XYZ and Lab are separate models with explicit
channel roles and white. Lab L* and a*/b* have their own units; no channel-name
guessing makes a field a color. The reference white is declared XYZ normalized
to Y=1; named D65 convenience construction fixes the same white in all callers.

Alpha association is none, straight or coverage-premultiplied. Alpha, when
present, is the final coverage channel in `[0,1]`. Coverage-premultiplied data
requires zero RGB at zero alpha; straight permits hidden colors. Unassociation
divides by every positive alpha without an epsilon, returns zero RGB at zero
alpha, and rejects overflow. Association at zero alpha loses straight hidden
color by definition. Nonlinear color conversion accepts unassociated color;
callers compose unassociate/convert/associate explicitly. XYZ/Lab conversions
use the same declared white; linear-sRGB↔XYZ uses D65. D50 XYZ↔Lab is supported
with an explicit white. No implicit chromatic adaptation or gamut clamp occurs.

Port constraints describe accepted dtype/shape, optional typed semantic kind/
descriptor and scalar bounds. Output semantics have explicit preserve, establish,
transform or drop behavior. Preserve copies the actual input facets; establish
and transform validate the resulting descriptor, and drop removes semantics
whose meaning the operation cannot guarantee. Typed outputs are carried in IR
and plans, validated after computation, and returned unchanged through caches.
Generic numeric arithmetic drops semantic guarantees; channel extraction emits
a field or coverage mask with the extracted role/unit. Channel merging
establishes an explicitly declared target image descriptor.
Its inputs may be generic HW, scalar fields or coverage masks; known roles/units
must match the corresponding target channel. Generic inputs acquire that meaning
only through this explicit target and sample validation, allowing extract, generic
arithmetic, then merge without preserving invalid guarantees. Metadata assignment
changes interpretation only and validates all target constraints, including
samples, before publication.

All eight operations migrate in C++, the C module, Metal, fixtures and examples:
`image.exposure_gain`, `image.opacity`, `image.gaussian_blur`, `image.mask`,
`image.source_over`, `image.downsample_box`, `mask.downsample_box`, and
`image.brush_circle`. Their image ports remain specifically linear D65 coverage-
premultiplied RGBA; mask ports use typed coverage masks. Existing bounds and
formulas remain, except brush RGB accepts finite signed values. New RGB/Lab
representations use their corresponding channel/color operations. Metal
eligibility must accept representative signed inputs and outputs: a blanket
`channel < 0` fallback is invalid. Subnormal/overflow limitations keep the
explicit numerical fallback and real-dispatch validation required by ADR 0019.

## G3: static descriptor inference

A port can declare an exact `element_type` or `element_type_mask`. The mask's
low four bits correspond to UInt8, Int64, Float64 and Float32 (element code minus
one); zero adds no restriction. Nonzero exact type and mask are mutually exclusive,
and unknown bits reject registration. The same sized C constraint field is copied,
validated and included in compiler/result identities. The unreleased ABI/Traits 7
target includes this field; numeric floating ports use mask 12 and binary operators
use a fixed two-member homogeneous group to enforce dtype/shape equality.

Output dtype and output shape are independent. Add an output dtype rule selecting
the declared dtype, an input dtype, or a validated static dtype parameter.
Preserve/match shape rules compare shapes independently of dtype. Explicit
output axes select a positive constant, a positive static Int64 parameter, an
input axis, or actual input count. Each source also has a nonnegative constant
offset (default zero), applied with checked addition; component capacity+1 uses
this field. Keep Scalar/Fixed/Shrink behavior where useful;
resolved descriptors, including facets, are computed once by a shared helper
before callbacks and revalidated on results. No runtime sample decides shape.

A registry operation may have a fixed prefix followed by one homogeneous
repeated input group with explicit minimum/maximum counts (active minimum>=1). Total inputs stay
within 1024; `channel.merge` uses 1..64 HW inputs of equal H/W and dtype. This
homogeneous group constrains shape/dtype; its members may be generic values,
scalar fields or coverage masks. Semantic lowering expands the group to the
exact ordered input table;
direct invocation performs the same count/schema expansion. The descriptor
and C ABI distinguish the template from the resolved table. Shape references
must resolve to a present input axis; repeated arity, static axis parameters,
and checked allocation products fail before callback/IR publication when invalid.

The closed `OperationSemanticRule` vocabulary also includes channel extraction,
selection, merging, alpha association changes and RGB/XYZ/Lab transformations.
These are shared descriptor rules, independent of operation keys or callbacks.
`MergeChannelsParameter` accepts explicit Image/VectorField/ComplexField targets
when the resulting HWC dtype/shape is valid. Known HW source roles/units match
corresponding target channels; names need not match. Image alpha extraction uses
canonical coverage metadata so it composes with existing exact mask ports.

`channel_indices_parameter` / `channel_indices_from_parameter` encode/decode a
canonical comma-separated decimal String: 1..64 indices, each 0..63, no spaces,
signs or leading zeros, at most 191 bytes. `IndexListCount` extends the extent
vocabulary using this shared parser; the same String drives swizzle metadata.
Selections preserving complete valid roles retain transformed typed semantics.
Repeated/missing roles, misplaced alpha or otherwise unrepresentable selections
produce generic output. Dropping straight alpha may preserve an unassociated
Image; dropping premul alpha must drop the image facet because RGB remains scaled.
Color conversion reads roles in any valid order and canonicalizes all output
channel names, including alpha `A`, while preserving alpha sample bits.
The unreleased ABI/Traits 7 vocabulary and existing identities include these rules;
WorkflowDocument schema 2 and provider ABI 1 remain unchanged.

The closed `SampleExpression`/`ApplyLut1d` rules share bounded expression parsing
and uniform-domain validation across compiler, direct calls and C declarations.
SampleExpression consumes generic Float64 `[K]` (1..256), requires finite start
and positive finite step, and validates resolved Float32 `[count]` (1..1048576).
Its output has dimensionless value/axis units; a multi-sample endpoint must be
finite and greater than start. ApplyLut1d accepts a SampledSignal query and
SampledSignal/Lut table with N>=2, matches query sample units to table axis units,
and drops output semantics. Both use Whole. See
[expression and LUT operations](../kernel-architecture/Expression-and-LUT-Operations.md).

## G5: computed scalars and reusable operation subset

An upstream generic Float32 `{1}` may feed a bounded scalar port. Compile-time
validation checks dtype/shape and known semantics. A bounded scalar accepts
unfaceted generic values, dimensionless scalar semantics, or a dimensionless
single-sample signal with complete `{1}` coverage. Dimensionless refers to the
sample value unit; the sampling axis domain/unit is independent and is retained.
Image, mask and unit-bearing field semantics are rejected. Declaration preflight
still checks every direct consumer's interval before any callback. Computed values
are checked for finite/range validity before each consuming callback, including
cache hits. Direct binding numerical errors are `InvalidArgument`; invalid
computed values are `OperationFailed`. Metadata mismatch is `TypeMismatch`.
Cancellation/currentness, callback-free rejection and no partial publication
retain their existing precedence. Bounds remain gain `[0,16]`, opacity `[0,1]`.
Per-Run bindings and mutable results never enter shared plans or registry state.

The following is the minimum named operation surface. Defaults are values that
workflow constructors explicitly submit; registry validation supplies none.
All new operations use Whole in this first implementation.

| Family | Fixed first-delivery behavior |
| --- | --- |
| `numeric.cast`, `numeric.encode_range` | Required target dtype; rounding `ties_even`, overflow `reject` or explicit `clip`; range source/destination endpoints required, dither off. Shape unchanged, generic output. |
| `numeric.add/subtract/multiply/divide`, `numeric.clamp` | Equal dtype/shape Float32/64 arrays; clamp has finite inclusive min/max, min<=max. Generic output; no broadcasting. |
| `numeric.mean`, `numeric.variance` | All samples reduced to generic Float64 `{1}`; population variance and fixed row-major two-pass accumulation. |
| `channel.extract/merge/swizzle` | Extract uses required zero-based index. Merge establishes an explicit target descriptor from generic HW/field/coverage inputs, validating known roles/units and target samples. Swizzle uses an explicit bounded index list, permits repeats, infers C and transforms roles; no implicit constants/resize. |
| `alpha.associate/unassociate`, `color.assign` | Association changes only color channels and the descriptor; alpha bits preserved. Assign has an explicit target descriptor, preserves sample bytes and validates its declared domain. |
| `color.rgb_to_xyz/xyz_to_rgb/xyz_to_lab/lab_to_xyz` | Float32 HWC RGB/XYZ/Lab, optionally straight alpha; binary64 coefficients/intermediates, Float32 output, unchanged alpha. Same declared reference white, signed/out-of-gamut values preserved. |
| `numeric.sample_expression` | Static bounded expression, Float64 start/step and Int64 count>=1; step>0, Float32 `[count]` output with sampled-signal semantics. Dynamic coefficients are Float64 `[K]`, K>=1; no per-run shape changes. |
| `lut.apply_1d` | Float32 sampled signal plus Float32 `[N]` SampledSignal/Lut table with uniform sample-axis semantics (N>=2); linear interpolation, out-of-domain `reject` default or explicit `clip`; result keeps query shape and drops input semantics. No 3D LUT or channel-coupled interpolation. |
| `mask.threshold` | Finite Float32 HW scalar field to typed coverage mask; threshold .5 explicitly supplied, comparison `>=`. |
| `mask.components`, `component.count/area/bbox` | Binary typed mask to Int64 HW labels, then separate count/area/bbox nodes. Four-connectivity, row-major first-pixel labels 1..Kcap, background 0. Capacity Kcap>=1 is static; overflow fails. Count Int64 `{1}` counts foreground components; area Int64 `[Kcap+1]`, bbox Int64 `[Kcap+1,4]` in x_min,y_min,x_max_exclusive,y_max_exclusive order. Background and unused records are zero. |

Component capacity is independently declared by each node in the existing exact
bounded Int64 range `[1,2^53-1]`. Labels use canonical ScalarField metadata:
Int64 HW, name/role `component_label`, dimensionless value/channel units, no
capacity facet. Attribute ports require these exact facets and validate every
label in `[0,capacity]`; sparse IDs are legal and count means distinct nonzero
IDs. A producer with a larger declared capacity may feed a smaller consumer when
actual IDs fit. Capacity limits final components, including merged bridges.
Count's accounted deduplication workspace is bounded by input samples, not capacity.
See [component operations](../kernel-architecture/Component-Operations.md).

Expression grammar is limited to decimal/scientific numeric literals (no hex,
NaN or infinity names), x, `c[index]`, parentheses,
unary +/- and binary +,-,*,/,^, with pure `abs`, `sqrt`, `exp`, `log`, `sin`,
`cos`, `min`, `max` calls. Exponentiation is right-associative and binds above
unary negation; `0^0=1`. Every subexpression must be finite. Limit UTF-8 source to 4096 bytes, AST to 256 nodes/depth 32,
coefficients to 256 and count to 1,048,576; validate coefficient indices against
the declared table. Reject domain errors/non-finite results with a sample index;
no loops, scripts, filesystem or hidden mutable state. Reuse one bounded parser
for validation/evaluation; AST and parameters enter identities, coefficient
bytes enter only content identities. Cooperative cancellation and accounted
output/scratch allocations apply to all loops and component work queues.

## Snapshot, cache and version integration

InputSnapshot import/read/patch accepts the supported typed v2 image descriptors
and HWC C from the descriptor instead of hard-coding four channels. Patches keep
the complete descriptor/facets, validate samples, and use checked C-scaled byte
offsets. A frozen execution remains an in-memory owned plan; it retains its
registry/snapshot rules and does not add a serialized plan format.

Compiler identity includes every semantic descriptor, input and output constraint,
dtype/axis/semantic inference rule, repeated-input bounds/resolved count, and
static parameter. Result-region keys additionally include full resolved traits,
descriptors/facets, operation implementation, exact input content/dependencies,
requested coverage, numeric mode and backend/device identity where applicable.
Bounds or output meaning cannot change while retaining a reusable key. Runtime
binding order, native pointers, times and allocation ids remain excluded.

Regional result keys also accept preflight-validated dense, offset-zero direct
Values up to 2048 bytes when the derived demand covers the complete Whole value.
Snapshot, bounded-scalar and compact whole-Value sources have distinct category
tags. Compact keys include dtype, rank/shape, exact facets, byte length and raw
bytes, including signed-zero bits and unused coefficients. Larger/partial direct
inputs remain unproven. This qualification applies to regional execution and
execute_stream; pure generic/scalar ordinary execute keeps its existing fast
path. No new snapshot/disk types are introduced. The public expression workflow
checks 2048/2049+ boundaries, dtype/shape/facet separation, concurrent coefficients
and cached invalid bounded consumers.

Memory/native/disk cache entries preserve actual output facets. Validate stored
metadata against expected output semantics and validate numerical consumer
constraints on hits; never reconstruct the old hard-coded image facet. Bad disk
format/checksum/metadata is a miss, not a fabricated typed Value. Only validated
successful current results publish. Preserve cancellation, in-flight ownership,
cache budget and GPU disk-exclusion rules.

Target versions are package **0.7.0**, operation ABI/OperationTraits **7**,
semantic/physical-plan/plan-cache domains v7 and result-region-key v3. The
optimizer stays `optimizer-v5-canonical-noop` because its rule is unchanged;
the semantic input changes its digest. Result digest framing remains v2 because
it already encodes complete facets and samples. Disk-derived data moves to v2
and old entries become misses without a legacy reader. Schema **2**, provider
ABI **1** and C++17 remain. C++ consumers rebuild; operation ABI 6 and package
0.6 requests are rejected. Daemon's 0.6 consumer requires separate migration.

## Delivery and acceptance

Develop from the synchronized baseline on `ops-foundations`, based alongside
`ops` on main. The only implementation PR is `ops-foundations` into `ops`, using
a merge commit and preserving one commit per completed development Issue.
Keep local/remote `ops`; remove only `ops-foundations` after verified delivery.
Main receives no merge. The ordered leaves are:

| Issue | Completion boundary |
| --- | --- |
| #288 | This accepted English contract, Chinese mirror and target/fact separation. |
| #289 | Shared typed semantics, static output/repeated-input inference, C/C++ ABI and identities. |
| #290 | All eight operations, C module, Metal, existing examples and regressions migrated. |
| #291 | Input snapshots, freeze and complete memory/native/disk cache semantics migrated. |
| #292 | Computed scalar normal/error/cache/concurrent/cancelled paths. |
| #293 | Numeric cast/range/clamp/arithmetic/reduction operations and focused oracles. |
| #294 | Channel, alpha and explicitly white-referenced color combinations. |
| #295 | Bounded expression, dynamic coefficients and linear 1D LUT. |
| #296 | Threshold, stable labels and fixed-capacity separate attributes. |
| #297 | Installed public workflow examples and combined acceptance. |

`examples/foundations_workflow` must compile and execute through installed public
WorkflowDocument/compile/execute APIs, with commands and independently checkable
outputs. Cases include all 256 UInt8 values round-tripped through range encoding,
halfway/overflow/non-finite conversion, a negative descending ramp, channel
identity/red-only scaling, legal alpha round-trip/zero hidden-color loss/tiny
alpha, signed/HDR and D65/D50 Lab round-trips. Wrong units/facets/association fail.
Expression x^2 samples at 0,.5,1 give [0,.25,1]; the linear LUT at .25 gives .125.
Different static counts reuse the same operation key. Dynamic coefficients reuse
one plan sequentially/concurrently and feed gain without binding leakage; illegal
fresh/cached scalars never enter its callback. Empty foreground returns zero
labels/count and nonempty zero attribute tables; include diagonal connectivity,
tile-boundary components, stable order and capacity errors.

Each implementation slice runs focused validation, ClangFormat 21/cpplint for
changed C/C++, and relevant isolated static/shared public consumers. Eight-op
CPU/C-module/Metal comparisons retain ROI/tile/resource/cancellation regression
coverage. A representative signed Metal case must observe native dispatch.
After all leaves, a fresh independent comprehensive review precedes pushing,
the PR's six Linux/macOS static/shared/ASAN/TSAN CI jobs, Codex review-bot fixes
and revalidation, merge into ops, Issue settlement and branch cleanup. Unavailable
external review is reported as a blocker, never as completed acceptance.
