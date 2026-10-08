---
spec_schema_version: 1
id: FMT-06
kind: shared_operator_contract
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_result_abi_2
clarification_status: current_member_complete_extensions_pending
---

# FMT-06: numerical dtype and interval conversion

The default registry provides one native Result ABI 2 key,
`numeric.convert_format_strict`. It accepts one single-tensor Result with no
fields and publishes one `values` output Result containing one tensor. It supports
all 49 source/target pairs among UInt8, UInt16, Int8, Int16, Int64, Float32 and
Float64. The operation uses
the exact numeric contract below and is registered only under this strict key;
SIMD and eligible SME paths are internal implementations.

This family and its member remain Proposed. That design status is independent of
the current registered implementation. The operation preserves Result schema
identity, tensor key, logical shape and batch axes. It updates the tensor dtype,
encoding facets and `atomic_trailing_axes` for the conversion. It reports full sample rank
including batch and cell axes at most 8, and a full sample count at most 2^40.

Inherit [FMT-common](FMT_common_contract.md), the NUM numerical baseline and
[kernel storage](../../../kernel-specs/Tensor-Storage-and-Region-Access.md).
The single member is [A convert numeric format](FMT-06A_convert_numeric_format.md).
The default conversion rescales dtype-selected intervals; explicit
`rescale=false` requests a pure numerical cast. No bit reinterpretation occurs.

## Purpose, scope and implementation boundary

UInt8 255 -> Float32 defaults to 1, while disabling scaling yields 255.
Neither mode implicitly changes gamma/transfer, color model, primaries, alpha
association, pixel coordinates, file packing or byte order. Target dtype applies
to the whole tensor; logical shape and channel order are unchanged. Different
channel dtypes require separate tensors. A complete image remains straight,
with any alpha referenced inside the same tensor.

Basic interval remapping belongs to this family. FMT-07 is retired. Effective
bit-depth rules, legal code sets and encoding/decoding validation remain allocated
to future FMT-06 members; their interfaces are outside this current member.
Dither and halftone belong to GRD-29/30, and opaque-alpha generation is specified
in FMT-05B with explicit or dtype-default encoding. This member does not
reinterpret color, alpha or transfer semantics beyond its declared numeric
encoding conversion.

## Confirmed decisions

| Topic | Selected behavior |
| --- | --- |
| Family | A numerical dtype plus interval conversion; no bit reinterpretation. |
| Target dtype | One static dtype for the whole tensor; preserve shape/axes/channel order. |
| Scaling | Enabled by default with dtype-selected intervals; explicit disable gives pure cast and forbids range parameters. |
| Intervals | Shared pairs or complete static per-channel tables with an explicit channel axis; independent source/target defaults or overrides. |
| Bounds | Finite endpoints; source lower<upper; target endpoints differ and may be reversed. |
| Input outside source interval | Extrapolate; no implicit clipping to interval endpoints. |
| Overflow | Round first; reject by default, optional clip to destination dtype endpoints. |
| Nonfinite source | Integer target rejects NaN/Inf even with clip; floating targets use mapped Inf and deterministic NaN propagation. |
| NaN width conversion | Preserve sign/high payload bits, widen with low zeros, narrow by discarding low bits, quiet nonidentity conversion. |
| Identity | Same dtype plus static identity map copies exact bits, including signaling NaN and signed zero. |
| Metadata | Respect checks declared encoding against resolved source ranges; preserve applicable semantics and update encoding/decoding relations. |
| Layout | auto default; a same-dtype identity over the complete declared mapping may use a legal generic or spatial view; otherwise materialize, and forced view reports ViewUnavailable. |

## Types and logical parameters

All 49 source/target pairs among UInt8, UInt16, Int8, Int16, Int64, Float32 and
Float64 are supported. Float16/Int32/UInt32/UInt64 are outside this member. No
mixed-channel dtype, runtime-dependent dtype or implicit promotion is supported. Complete sample rank,
including batch and cell axes, is at most 8; complete sample count is at most
2^40 with checked positive extents and offsets.
Generic tensors and planar images share the numeric formula; labels do not
relax actual image storage requirements.

| Dtype | Default source / target interval |
| --- | --- |
| UInt8 | [0,255] |
| UInt16 | [0,65535] |
| Int8 | [-128,127] |
| Int16 | [-32768,32767] |
| Int64 | [-2^63,2^63-1] |
| Float32 / Float64 | [0,1] |

Choose defaults from dtype, not observed extrema or model names. Int8 zero thus
maps to 128/255 for floating output; signed zero is not a special range anchor.
Use exact integer limits, including Int64 maximum, never rounded Float64 limits.

These static parameters use the registered operation's typed parameter codec:

| Parameter | Definition / authoring default |
| --- | --- |
| dtype | Required target type, using the seven lowercase names corresponding to the table. |
| rescale | Bool, true by default. False rejects explicit range fields/axis tables. |
| source_range / target_range | Optional typed endpoint pair or complete per-channel pair table; omission independently selects that side's dtype default. |
| axis | Required nonnegative Int64 cell axis if either range is a table; excludes the Result batch prefix. Assert consistency with effective metadata when present; raw supplies the axis directly. |
| rounding | ties_even only, inherited default; other modes require explicit preceding NUM operations. |
| overflow | reject by default, or clip. No wrap mode. |
| metadata_mode | respect by default; override or raw explicitly. |
| metadata_override | Effective input interpretation only with override; cannot alter physical shape/dtype/storage. |
| layout | auto by default; view or materialize. |

An endpoint is an exact finite typed numeric constant, including the represented
value of a floating literal. Typed endpoint tables retain exact integer and floating-bit representations; do not encode them as untyped Double values. Bounds are static,
not per-pixel inputs or data-dependent statistics. Table length equals the channel
extent, with no implicit repetition, sparse guessing or role-based matching.
A shared source with tabulated target, or the reverse, is allowed. Component
inputs use shared intervals. Preflight validates every table entry even if its
channel samples are unrequested. Source endpoints must increase; target endpoints
may decrease but cannot coincide. A constant target belongs to NUM-06B/constant
operations, not this encoding conversion. Signed-zero-equal endpoints are equal.

## Mathematical reference and conversion stages

For finite exact source value x and resolved bounds s0<s1 and t0!=t1:

```
rescale=true:   v = t0 + (x-s0)*(t1-t0)/(s1-s0)
rescale=false:  v = x
result:        round v to destination, then apply reject/clip if it overflows
```

Strict rounds this entire mathematical expression directly to the target once.
Integer source and endpoint arithmetic is exact. No intermediate Float32/Float64
conversion defines the reference. Use exact arithmetic or a certified equivalent;
intermediate machine overflow is not an output overflow when exact v is finite.
Resource exhaustion remains an explicit failure, not permission to approximate.

For integer output, round v to an unbounded mathematical integer using
nearest/ties-even, then test target integer bounds. Reject an out-of-range result
or clip to the corresponding exact bound. Do not wrap or use an undefined
machine cast to test overflow. Pure cast -0.5 -> UInt8 succeeds as 0; 255.5
rounds to 256 and then rejects or clips to 255.

For floating output, correctly round v to the target format with gradual
underflow. Finite results succeed, including inexact results and sources just
outside the finite target interval that round back to a finite value. A rounded
signed infinity from finite v rejects or clips to signed maximum finite under
clip. Do not reject merely because exact v exceeds the maximum finite sample.
Floating underflow remains successful. Ordinary NUM arithmetic overflow behavior
is unchanged by this explicit checked conversion contract.

In rescale mode, exact input source endpoints select their corresponding target
endpoint values before final conversion. Endpoint floating zero signs are retained
where conversion permits. Ordinary exact-zero affine results use NUM's +0 rule;
pure floating casts preserve source zero sign. True identity paths below take
precedence over remap endpoint arithmetic. All static validity/metadata checks
still apply, including for identity requests.

A same-dtype disabled conversion, or same-dtype pair with equal corresponding
source/target bounds, is a bit copy. Endpoint zero signs must agree for a
same-interval identity proof; differing explicit zero signs require the remap
endpoint rule. Identity may apply to selected channels of a materialized result,
but a view requires identity for the whole declared tensor mapping. No sample
scan or coincidence of observed values establishes identity.

## Nonfinite samples and NaN representation

If the destination is integer, a NaN or either infinity in the original source
fails InvalidDomain even under clip. Clip handles finite value overflow only.
For floating output, source infinity follows the sign of the exact affine slope,
or preserves sign with scaling disabled; it remains infinity rather than being
clipped as if it came from a finite overflowing result. Nonidentity source NaN
propagates under the deterministic mapping below. No color validity scan is added.

Binary32 and binary64 have 22 and 51 payload bits excluding the quiet bit.
Preserve NaN sign, left-align payload bits to the destination field, shift left
by 29 on widening or right by 29 on narrowing, and set the target quiet bit.
Same-width nonidentity mapping preserves payload/sign and sets the quiet bit.
Never round discarded payload bits. If all retained payload bits vanish, the
set quiet bit still produces NaN. Same-dtype static identity copies every bit,
including signaling NaN, without quieting. Mapping is exact across CPU implementations.

Pure dtype casts use correctly rounded conversion. Affine transforms round the
exact expression once. The strict key may select SIMD instructions when their
results satisfy these rules exactly; there are no separate accelerated keys.
Integer outputs, discrete choices, endpoint/copy rules, NaN payloads and
reject/clip/nonfinite classifications are identical across implementations.
Lanes outside a proven SIMD domain use the exact scalar path. Preserve the
caller's floating environment. ISA dispatch is an implementation detail of the
strict operation and does not relax its numerical contract.

## Metadata authority and propagation

In respect mode, an explicitly declared input numeric encoding interval must
agree with the resolved source interval, including when a default was selected.
For Float32 described as 0..255 codes, default 0..1 therefore fails; explicitly
supply [0,255] or change effective interpretation through override/raw. Do not
silently use metadata in place of the selected default. Interval assertions
compare lower/upper numeric bounds; orientation lives in the decoding map.
Thus a prior reversed target [1,0] has stored bounds [0,1] and a reversed decoder,
not an illegal descending source interval for the next conversion.

Preserve applicable color model, primaries/white/transfer, straight alpha
references, coordinates, names/roles and units. Update dtype-dependent storage
fields and each component's encoding/decoding relation. If source decoder is
D and the affine map is f, the target decoder is D composed with inverse(f).
This is an encoding correspondence, not a guarantee to recover samples lost by
rounding or clipping. Native coordinates use an identity decoder when no explicit
encoding exists. Do not claim that alpha byte 255 is mathematical coverage 255.
Consumers requiring normalized alpha or native color coordinates must decode
encoded output explicitly; a dtype-only float cast does not perform that step.

Scaling disabled preserves applicable source decoder/code interval while
changing dtype. No source-range consistency check applies because no mapping
range is selected. In all cases drop stale sample-validity guarantees; do not
validate complete RGB/alpha just because descriptions exist. Raw ignores semantic
source assertions, retains only applicable descriptions/provenance and removes
encoding/group claims whose propagation is invalid under that reinterpretation.
Override changes effective source fields for this call, never another consumer.
Neither mode creates an external alpha relation or a premultiplied canonical image.

## Exact demand, layout and invalidation

For requested coordinates Q, Data is exactly input[Q]. Value conversion checks
only those samples, including nonfinite integer errors or overflow. Do not read
other channels, scan global extrema or close over complete color groups. All
static dtype/shape/bounds/consumed metadata belong to Descriptor support and are
checked independently of Q. There is no sample-derived Control dependency.
Dirty samples map to identical logical output coordinates. Static intervals,
mode, dtype or applicable metadata changes invalidate inference/results; no
old success evidence survives a changed mapping. Required upstream Whole work
retains its original failure scope.

Auto/view first requires same dtype and a statically proven identity for every
channel, plus a legal immutable same-owner mapping under kernel rules. Forced
view fails ViewUnavailable when this cannot be proved, even if Q happens to lie
only in identity channels. Auto materializes otherwise. Materialize always
reserves a new output and produces exactly Q, including for identity. Do not
reinterpret source bytes or create a multi-owner image as a conversion shortcut.
Identity view can retain a large existing backing but does not publish or read
unrequested coverage merely because source bytes are available.

## Reference algorithm, resources and errors

Resolve modes/types/intervals and validate every static table/description. Build
bounded per-channel constants and mapping/decoder metadata. Map Q to identical
source coordinates, acquire admitted read windows, prove any identity view or
reserve output with target-width layout and prepare required pages. For each
requested sample, copy identity bytes or classify/transform/convert according
to the reference. Check cancellation/currentness and publish only successful
requested coverage. Static table preprocessing does not scan source pixels.

Images, including encoded results, obey planar storage, DAG tile geometry,
edge-row padding, page-aligned tile starts and one full-image virtual range.
Provided output backing scales with target sample width and admitted pages,
not the source's stride bytes. Generic non-image tensors use admitted storage.
Logical bytes, virtual capacity and physical RSS are distinct.

For rank r, C interval entries, Nq samples and F windows, mapping work is
O(C+Nq*r+F*r), plus exact rational/integer arithmetic and page-set work. Static
finite endpoint widths and admitted numeric scratch bound each conversion;
charge limb capacities/work to shared budgets and fail rather than approximate
when insufficient. Do not allocate a full-image temporary or eager per-page
directory. Coalesced/SIMD accesses remain inside requested spans and prepared
pages. Use host scheduling and no private pool. Poll cancellation/currentness
at most every 64 samples or bounded exact-arithmetic steps, during large table
processing and immediately before publication, following NUM remap discipline.

Account simultaneous input/output owners, actual provided pages, windows,
per-channel exact constants, encoding metadata and scratch; deduplicate retained
owners. Views retain normal source capacity. Failed unpublished output releases
resources without revoking prior observations. No eviction, replay or metadata
alpha capture is introduced. Result caching is disabled. Empty output demand is stateless. A run reads only
requested source coordinates, prepares output under Root resource limits, checks
cancellation/currentness, and publishes successful coverage transactionally.
Failures preserve source status and diagnostics; unpublished owners are released.
Identity view retains its normal source owner and capacity. All layouts, including
spatial storage, may use a view only when the complete declared mapping is a
legal same-dtype identity; otherwise materialization writes target-width storage
for requested pages. Data uses role 1 and Descriptor uses role 8; the operation
requests no Validation or Control support.

| Failure | Phase | Status / reason |
| --- | --- | --- |
| Missing/malformed parameter, unsupported rounding/mode, invalid axis/table, nonfinite/invalid/degenerate bounds, source-encoding conflict | Compile/direct preflight | InvalidArgument / InvalidDomain. |
| Unsupported dtype/rank or structurally incompatible effective descriptor | Static preflight | TypeMismatch / None. |
| Original source NaN/Inf with integer target | Requested evaluation | OperationFailed / InvalidDomain. |
| Finite rounded target overflow with reject | Requested evaluation | OperationFailed / ArithmeticOverflow. |
| Forced view with a non-identity declared mapping | Compile/direct preflight | InvalidArgument / InvalidDomain; ViewUnavailable. |
| Forced view for an identity mapping whose source physical representation cannot express the requested view | Observation evaluation | InvalidArgument / InvalidDomain; ViewUnavailable. |
| Checked shape/address overflow, missing coverage, budget/work, unsupported backend, cancellation/stale or required producer failure | Inherited phase | Preserve NUM/kernel code, origin and scope. |

Clip publishes the appropriate target dtype endpoint rather than adding a warning
output. A sample conversion failure preserves its code, reason and coordinate, and
adds `source_dtype`, `target_dtype` and `range_index`; for a tabulated range it also
adds the cell-relative `channel_axis` and selected `channel`. `range_index` refers
to the static interval table. These fields are added for numerical failures in
`convert_one`; upstream, resource and cancellation diagnostics keep their original
content.
The operation is registered as one CPU strict key using Result operation ABI 2.
Eligible contiguous spans use internal AArch64 NEON or runtime-checked amd64 AVX2;
eligible Apple arm64 Float32→UInt8 spatial tiles may use the internal SME path.
These paths preserve the exact same numerical behavior. No GPU backend is provided.

## Acceptance and implementation requirements

The member lists independent analytic cases. Compare exact integer/rational
oracles with strict rounding and use explicit bit patterns for NaNs/zero. Test all
49 pairs, endpoints, outside-interval extrapolation, narrowing, large Int64 values,
per-channel maps, metadata conflict/override/raw, exact regional failures, static
invalid unrequested table entries and whole/disjoint/tile request equivalence.
Verify view/materialize, context-independent owners, low budgets, cancellation,
currentness, floating environment and declared fallback diagnostics.

Six focused CTest targets pass: `test_numeric_conversion`,
`test_numeric_conversion_sme`, `test_alpha_numeric_interop`,
`test_alpha_operations`, `test_alpha_authoring` and `test_resources`. The numeric
suite covers 49 dtype pairs, randomized oracles, ROI, cold-lookup budget,
failure-order, floating-environment and identity views. Additional batch/view/
Empty/bit-stride, ICC-resource, concurrent-plan-reuse and payload-release checks
pass. `test_alpha_numeric_interop` covers four inherited/moved cases. These checks establish behavior, not
performance. Thirteen Result performance smoke cases pass their byte oracle, but
their timings have not been interpreted as performance evidence.
