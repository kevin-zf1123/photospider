---
spec_schema_version: 1
id: FMT-06A
parent_id: FMT-06
function: convert_numeric_format
kind: primitive
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: complete
registered_operation_keys:
  - numeric.convert_format_strict
---

# FMT-06A: convert tensor dtype and numeric interval

Inherit the complete [FMT-06 contract](FMT-06_numeric_conversion_contract.md).
The default registry provides `numeric.convert_format_strict` as a CPU fixed invocation
ABI 8 operation. It takes one single-tensor Result and produces one `values` output
Result containing one tensor. Its default scales numeric intervals while converting dtype;
explicit `rescale=false` gives a pure numerical cast. The specification remains
Proposed.

## Interface and observable behavior

One `input` tensor yields one `values` tensor of required static target dtype.
Supported source and target names are uint8, uint16, int8, int16, int64, float32
and float64.
Output rank/extents/axes/channel order equal input. One target type covers the
whole tensor. Inherit rescale, source_range, target_range, conditional axis,
rounding, overflow and layout from the family. Range endpoint parameters use typed exact encodings; defaults follow NUM.

Choose omitted interval sides from dtype. Explicit pairs or full channel tables
override only their side. Table selectors are static channel indices on a declared
axis, not runtime pixel-dependent intervals or fuzzy channel roles. Equal target
endpoints are invalid; reversed targets are allowed. Source bounds increase.
When rescale=false, interval fields/axis tables are invalid rather than ignored.
No input description or attached encoding is read.

For full sample coordinate q, determine the resolved pair from
q[batch_prefix_length + axis] if tabulated, otherwise the shared pair. Read only
x=input[q]. In strict, compute the exact
affine rational value (or x for pure cast), then round once to the destination.
Clip finite output overflow after rounding; reject requires a representable
rounded result. Integer targets require finite original source samples. Floating
NaN mapping and infinity signs follow the family.
The caller supplies any target encoding to later consumers explicitly.

## Mapping, resources and error specialization

The compiler retains complete output tiles and reads their closure in the input's
own tile grid. Each output coordinate uses the corresponding source coordinate;
no additional alpha/color peer or global extrema scan is needed. All static
entries are validated, including unselected channels. Caller numerical premises
cover every coordinate evaluated in the retained output tiles.

Auto and materialize write independent dense output tiles, including identity.
View requires a statically identical map across the complete declared tensor;
nonidentity view fails static preparation. A nonempty identity view has only a
schema dependency and fails ViewUnavailable without reading input payload.
Empty demand has no tasks. Shape, batch axes and spatial topology are preserved;
output dtype changes and `atomic_trailing_axes` becomes zero.

Nonidentity kernels use the family's fixed exact arithmetic scratch. Identity
and view kernels declare zero scratch. Inputs and prepared maps are immutable;
output allocations and scratch belong to the invocation. Operational failures,
retirement, cancellation and owner lifetime follow the fixed executor contract.

## Independent numeric fixtures

1. UInt8 [0,128,255] -> Float32 with defaults gives
   [0,RN32(128/255),1]. With rescale=false, it gives [0,128,255].
   These modes must have different explicit parameters and compiled invocation identity.
2. Int8 [-128,-1,0,127] -> UInt8 with defaults gives [0,127,128,255]
   exactly. Int8 zero -> Float32 gives RN32(128/255), not zero.
3. Int64 [minimum,-1,0,maximum] -> UInt8 defaults gives [0,127,128,255].
   The exact mapped value for -1 lies just below 127.5; prematurely rounding to
   Float64 produces 127.5 and the incorrect ties-even integer 128. This fixture
   detects an intermediate-double implementation and rounded Int64 endpoints.
4. Float64 [-0.5,0.5,1.5,2.5,255.5] -> UInt8 with rescale=false:
   reject has valid inputs for the first four, yielding [0,0,2,2]; the last
   violates its representability premise;
   clip gives [0,0,2,2,255]. Negative input alone is not an overflow before rounding.
5. With Float64 source [0,1], Float32 target [1,0], inputs [0,0.25,1,2]
   give [1,0.75,0,-1]. No clipping to target interval occurs. Target [0.5,0.5]
   fails static bounds validation even when its channel is unrequested.
6. The next Float64 value above Float32 maximum finite converts successfully
   to Float32 maximum finite when mapping is identity. A sufficiently large
   finite value that rounds to infinity violates the reject premise, or clips to maximum finite.
   Source +Inf remains +Inf for floating output even under clip; integer output
   requires finite sources under either overflow policy.
7. Float32 nonidentity NaN 0xffc12345 widens to Float64 0xfff82468a0000000.
   Float64 signaling NaN 0xfff0000000000001 narrows to quiet Float32 0xffc00000.
   Same-dtype static identity preserves both signaling and quiet source bits.
   Verify bit patterns independently, not through a platform floating cast.

For same-dtype identity, include -0, +/-Inf, multiple NaN payloads and valid
negative/zero-stride generic inputs. For nonidentity transforms use independent
exact rational rounding plus NUM's endpoint/zero reference; pure casts never
route Int64 through Float64. SIMD implementations inside the strict key must
match the same exact result, including tails and exceptional lanes.

## Metadata and channel-range fixtures

A Float32 normalized-Lab+alpha tensor can use source ranges [0,1], [-128,127],
[-128,127], [0,1] on an explicit channel axis and target UInt8 defaults.
Samples [0.5,0,0,0.5] then become [128,128,128,128]. A later consumer may use an explicit Lab/coverage description with a
corresponding per-channel decoder; the byte 128
in the alpha channel is not mathematical alpha 128. This illustrates explicitly
selected encoding ranges, not a universal Lab device encoding standard.

For Float32 samples interpreted by the caller as codes over [0,255], an
explicit `source_range=[0,255]` with target Float64 defaults maps 255 to 1.
Omitting the source range uses [0,1] and leaves 255 as 255 for Float64 output.
Disabling scaling also preserves the code 255. None of these paths propagates a
decoder or checks a hidden source interpretation.

A reversed target retains ordered code bounds and decoder orientation. An
explicit inverse range conversion can reverse it back; source_lower must still
be less than source_upper. Quantization/clipping is lossy, so do not demand an
exact arbitrary round trip. Defaults do not inspect data extrema or infer a
range from names such as alpha, normalized Lab l or red.

## Public workflow and current validation

The public graph is `declared input Result -> numeric.convert_format_strict ->
typed output Result`; consumers needing native color coordinates or normalized
alpha must decode explicitly.

Temporary numeric-conversion acceptance should cover all 49 dtype pairs using
independent exact references, complete/selected tiles, static lookup construction, operational failures,
floating environments, identity copies and forced-view failure, batch/view/Empty/bit-stride cases, concurrent plan reuse and payload release. Add explicit
alpha interoperability cases when changing inherited encoding behavior. An SME
path requires an actual dispatch check on admitted hardware; CPU output agreement
alone does not establish SME dispatch or performance.
