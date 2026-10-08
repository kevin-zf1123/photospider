---
spec_schema_version: 1
id: FMT-06A
parent_id: FMT-06
function: convert_numeric_format
kind: primitive
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_result_abi_2
clarification_status: complete
registered_operation_keys:
  - numeric.convert_format_strict
---

# FMT-06A: convert tensor dtype and numeric interval

Inherit the complete [FMT-06 contract](FMT-06_numeric_conversion_contract.md).
The default registry provides `numeric.convert_format_strict` as a native Result
ABI 2 operation. It takes one single-tensor Result with no fields and publishes one `values` output
Result containing one tensor. Its default scales numeric intervals while converting dtype;
explicit `rescale=false` gives a pure numerical cast. The specification remains
Proposed.

## Interface and observable behavior

One `input` tensor yields one `values` tensor of required static target dtype.
Supported source and target names are uint8, uint16, int8, int16, int64, float32
and float64.
Output rank/extents/axes/channel order equal input. One target type covers the
whole tensor. Inherit rescale, source_range, target_range, conditional axis,
rounding, overflow, metadata_mode/override and layout from the family. Range endpoint parameters use typed exact encodings; defaults follow NUM.

Choose omitted interval sides from dtype. Explicit pairs or full channel tables
override only their side. Table selectors are static channel indices on a declared
axis, not runtime pixel-dependent intervals or fuzzy channel roles. Equal target
endpoints are invalid; reversed targets are allowed. Source bounds increase.
When rescale=false, interval fields/axis tables are invalid rather than ignored.
Respect checks explicit source encoding assertions before reading samples.

For full sample coordinate q, determine the resolved pair from
q[batch_prefix_length + axis] if tabulated, otherwise the shared pair. Read only
x=input[q]. In strict, compute the exact
affine rational value (or x for pure cast), then round once to the destination.
Reject/clip finite output overflow after rounding. Original nonfinite input with
integer target fails; floating NaN mapping and infinity signs follow the family.
Update target encoding rather than pretending stored codes are native values.

## Mapping, resources and error specialization

Data and dirty mapping are exactly q<->q. All static table entries/consumed
metadata contribute to Descriptor checks, even if their channels are unrequested.
No extra alpha/color peer read or global min/max scan occurs. Requested conversion
failures stay within their inherited observation scope; required upstream Whole
failures retain their original broader scope. No error at an unrequested source
sample is manufactured by this operation.

View requires same dtype and statically identical mapping over the complete
declared tensor, plus a legal source owner/layout. A request restricted to one
identity channel cannot turn a generally transforming output into a forced view.
This identity view is supported for legal generic and spatial Result storage.
Materialization uses target-width planar storage for images and produces only
requested coverage.
The operation requests Data role 1 for source samples and Descriptor role 8 for
schema; it requests no Validation or Control support. Empty demand is stateless.
Materialization publishes transactionally under Root budgets and cancellation,
retains normal source/output owners, and preserves source diagnostics on failure.
Inherit exact arithmetic budget/work, backend/profile precision and error attribution.

## Independent numeric fixtures

1. UInt8 [0,128,255] -> Float32 with defaults gives
   [0,RN32(128/255),1]. With rescale=false, it gives [0,128,255].
   These modes must have different metadata and cache identity.
2. Int8 [-128,-1,0,127] -> UInt8 with defaults gives [0,127,128,255]
   exactly. Int8 zero -> Float32 gives RN32(128/255), not zero.
3. Int64 [minimum,-1,0,maximum] -> UInt8 defaults gives [0,127,128,255].
   The exact mapped value for -1 lies just below 127.5; prematurely rounding to
   Float64 produces 127.5 and the incorrect ties-even integer 128. This fixture
   detects an intermediate-double implementation and rounded Int64 endpoints.
4. Float64 [-0.5,0.5,1.5,2.5,255.5] -> UInt8 with rescale=false:
   reject succeeds for the first four as [0,0,2,2] and fails on the last;
   clip gives [0,0,2,2,255]. Negative input alone is not an overflow before rounding.
5. With Float64 source [0,1], Float32 target [1,0], inputs [0,0.25,1,2]
   give [1,0.75,0,-1]. No clipping to target interval occurs. Target [0.5,0.5]
   fails static bounds validation even when its channel is unrequested.
6. The next Float64 value above Float32 maximum finite converts successfully
   to Float32 maximum finite when mapping is identity. A sufficiently large
   finite value that rounds to infinity rejects, or clips to maximum finite.
   Source +Inf remains +Inf for floating output even under clip; integer output
   rejects source NaN/Inf under either overflow policy.
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
Samples [0.5,0,0,0.5] then become [128,128,128,128]. Model/component units remain
Lab/coverage with the corresponding per-channel numeric decoder; the byte 128
in the alpha channel is not mathematical alpha 128. This illustrates explicitly
selected encoding ranges, not a universal Lab device encoding standard.

A Float32 tensor explicitly encoded over [0,255] conflicts with default source
[0,1] in respect mode. Supplying source_range=[0,255] and target Float64 defaults
maps 255 to 1 and updates the decoder. Disabling scaling instead preserves code
255 and its existing interval. Check explicit override and raw separately so
ignored assertions never become stale validity evidence.

A reversed target retains ordered code bounds and decoder orientation. An
explicit inverse range conversion can reverse it back; source_lower must still
be less than source_upper. Quantization/clipping is lossy, so do not demand an
exact arbitrary round trip. Defaults do not inspect data extrema or infer a
range from names such as alpha, normalized Lab l or red.

## Public workflow and current validation

The public graph is `declared input Result -> numeric.convert_format_strict ->
typed, encoded output Result`; consumers needing native color coordinates or normalized
alpha must decode explicitly.

Six focused CTest targets pass: `test_numeric_conversion`,
`test_numeric_conversion_sme`, `test_alpha_numeric_interop`,
`test_alpha_operations`, `test_alpha_authoring` and `test_resources`. The numeric
suite covers 49 dtype pairs, randomized oracles, ROI, cold-lookup budget,
failure-order, floating-environment and identity views. Additional batch/view/
Empty/bit-stride, ICC-resource, concurrent-plan-reuse and payload-release checks
pass. The SME test passes, and `test_alpha_numeric_interop` covers four
inherited/moved cases.

`installed_numeric_conversion` and `installed_alpha_numeric_interop` pass; the
standalone numeric-conversion performance consumer configures and builds.
Thirteen serial Result performance smoke cases pass their bitwise output oracle.
A debugger-confirmed SME dispatch verifies the Float32→UInt8 tiled path is reached;
this is dispatch evidence, not a timing conclusion. The full performance matrix has
not run. Historical Value/planar measurements remain separate from current Result
behavior.
