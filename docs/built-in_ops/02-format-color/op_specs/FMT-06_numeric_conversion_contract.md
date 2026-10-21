---
spec_schema_version: 1
id: FMT-06
kind: shared_operator_contract
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: current_member_complete_extensions_pending
---

# FMT-06: numerical dtype and interval conversion

The default registry provides one CPU fixed invocation ABI 8 key,
`numeric.convert_format_strict`. It accepts one single-tensor Result with no
fields and publishes one `values` output Result containing one tensor. It supports
all 49 source/target pairs among UInt8, UInt16, Int8, Int16, Int64, Float32 and
Float64. The operation uses
the exact numeric contract below and is registered only under this strict key;
SIMD and eligible SME paths are internal implementations bound during preparation.

This family and its member remain Proposed. That design status is independent of
the current registered implementation. The operation preserves Result schema
identity, tensor key, logical shape and batch axes. It updates the tensor dtype and `atomic_trailing_axes` for the conversion. It reports full sample rank
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
| Overflow | Round first; reject requires representable rounded results as a caller precondition; clip clamps to destination dtype endpoints. |
| Nonfinite source | Integer targets require finite source samples under both overflow modes; floating targets accept mapped Inf and deterministic NaN propagation. |
| NaN width conversion | Preserve sign/high payload bits, widen with low zeros, narrow by discarding low bits, quiet nonidentity conversion. |
| Identity | Same dtype plus static identity map copies exact bits, including signaling NaN and signed zero. |
| Metadata | Preserve structural schema identity, tensor key, shape, batch axes and spatial topology; update dtype and clear atomic trailing axes. |
| Layout | auto and materialize write independent dense output tiles, including identity; nonempty view is unavailable. |

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
| axis | Required nonnegative Int64 cell axis when a range has more than one pair; excludes the Result batch prefix. A shared one-pair range needs no axis. Pure cast rejects axis. |
| rounding | ties_even only, inherited default; other modes require explicit preceding NUM operations. |
| overflow | reject by default, or clip. No wrap mode. |
| layout | auto by default; view or materialize. |

An endpoint is an exact finite typed numeric constant, including the represented
value of a floating literal. Typed endpoint tables retain exact integer and floating-bit representations; do not encode them as untyped Double values. Bounds are static,
not per-pixel inputs or data-dependent statistics. Table length equals the channel
extent, with no implicit repetition, sparse guessing or role-based matching.
A shared source with tabulated target, or the reverse, is allowed. Component
inputs use shared intervals. Preflight validates every table entry even if its
channel samples are unrequested. Source endpoints must increase; target endpoints
may decrease but cannot coincide. Integer endpoint zero has no sign, including
`i:-0`; floating endpoint tokens retain their explicit zero sign. A constant
target belongs to NUM-06B/constant
operations, not this encoding conversion. Signed-zero-equal endpoints are equal.

## Mathematical reference and conversion stages

For finite exact source value x and resolved bounds s0<s1 and t0!=t1:

```
rescale=true:   v = t0 + (x-s0)*(t1-t0)/(s1-s0)
rescale=false:  v = x
result:        round v to destination, then apply the selected overflow policy
```

Strict rounds this entire mathematical expression directly to the target once.
Integer source and endpoint arithmetic is exact. No intermediate Float32/Float64
conversion defines the reference. Use exact arithmetic or a certified equivalent;
intermediate machine overflow is not an output overflow when exact v is finite.
Resource exhaustion remains an explicit failure, not permission to approximate.

For integer output, round v to an unbounded mathematical integer using
nearest/ties-even, then test target integer bounds. With reject, the caller
guarantees the rounded result is in range. With clip,
clamp to the corresponding exact bound. Do not wrap or use an undefined
machine cast to test overflow. Pure cast -0.5 -> UInt8 succeeds as 0; 255.5
rounds to 256, which violates the reject precondition and clips to 255.

For floating output, correctly round v to the target format with gradual
underflow. Finite results succeed, including inexact results and sources just
outside the finite target interval that round back to a finite value. A rounded
signed infinity from finite v violates the reject precondition; clip returns
signed maximum finite. An exact v above the maximum finite sample can still
round to a finite target value. Floating underflow remains successful. These
requirements concern conversion to the destination format, independently
of ordinary NUM arithmetic.

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
and the output still owns independent storage. No sample scan or coincidence of observed values establishes identity.

## Nonfinite samples and NaN representation

If the destination is integer, the caller supplies finite original source
samples under both reject and clip. Clip handles finite value overflow only.
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
clip and nonfinite result rules are identical across implementations.
Lanes outside a proven SIMD domain use the exact scalar path. Preserve the
caller's floating environment. ISA dispatch is an implementation detail of the
strict operation and does not relax its numerical contract.

## Explicit numeric ranges and downstream interpretation

This operation accepts no description or metadata-mode parameter. It chooses
source and target ranges solely from explicit range parameters or the dtype
defaults above. A caller treating Float32 samples as 0..255 codes must supply
`source_range=[0,255]`; the operation cannot discover that interpretation from
the input. Omitting it selects the Float32 default [0,1].

The result carries structural schema and converted samples. It does not carry
an updated encoding, decoder, model, alpha association or sampling description.
Callers configure each downstream consumer explicitly. If a source decoder is
D and the conversion map is invertible f, D composed with inverse(f) describes
the ideal output encoding; the caller may use that relation when constructing
its configuration. Rounding and clipping can lose information, so this relation
is not a promise of exact sample recovery. A pure float cast does not decode
integer-like alpha codes into normalized coverage.

## Fixed Plan execution, layout and caller preconditions

The compiler emits one `convert` task for each retained output tile. The task
reads the closure of that tile's logical coordinates in the input's own tile
grid. Batch axes, atomic trailing axes and spatial topology can produce different
input and output grids. The kernel evaluates every coordinate in the complete
retained output tile. It does not evaluate additional samples merely because an
input tile grants a larger region. Static range tables are checked in full,
including entries for channels absent from the retained output tiles.

The unchecked numerical contract requires finite original sources for integer
targets, and representable correctly rounded results under `overflow=reject`.
These premises cover every evaluated output-tile coordinate. They do not require
inputs to lie inside the declared source interval. Floating NaN and infinity
inputs retain their defined floating-output behavior. Violating a premise has
unspecified numerical behavior and diagnostics; no first-error coordinate,
validation scan or numerical certification is promised.

Auto and materialize both write distinct preallocated, dense row-major output
tiles. Spatial metadata records logical topology independently of these physical
strides. Same-dtype identity copies exact bits into those outputs. View for a
nonidentity declared map fails static preparation. View for an identity map
lowers schema-only tasks and fails `InvalidArgument / ViewUnavailable` on
nonempty execution, without reading source payload. Empty demand has no tasks;
all static parameter and schema checks still apply.

## Exact arithmetic, resources and errors

Static preparation parses typed endpoints and constructs immutable per-channel
maps. Let finite endpoints and input be integers in units of `2^-1074`. Store
`W=S1-S0`, `A=T1-T0` and `B=T0*W-S0*A`. For an ordinary affine sample, round
`(X*A+B)/W * 2^-1074` directly to the target. Endpoint selection preserves the
target endpoint's zero sign. Pure casts preserve source zero signs; ordinary
exact affine cancellation yields positive zero. Identity copying takes priority.

The implementation uses 68 fixed 64-bit limbs. Endpoint magnitudes are below
`2^2098`, width/slope magnitudes below `2^2099`, and the affine numerator below
`2^4198`. Floating division alignment and 64-bit integer quotient rounding fit
within the 4352-bit capacity. Integer conversion uses unchecked representability
premises for the non-clipping policy. The explicit clipping policy compares
magnitude and applies ties-even before saturation as part of its result formula. Every mutable arithmetic temporary belongs
to an `ExactConversion` object in declared Host scratch. Static maps and the
UInt8-to-Float32 lookup are prepared before invocation; runtime arithmetic does
not allocate vectors or dynamic rational objects.

Each nonidentity task declares `sizeof(ExactConversion)` Host scratch, currently
3880 bytes on the supported CPU build. Identity and view tasks declare zero
scratch. Prepared maps and slot tables are immutable and reusable by concurrent
Runs; coordinates, cached read views and scratch belong to one invocation.
The executor reserves output and scratch capacity before issue, publishes calls
in retirement order, and drains accepted work on operational failure. Retained
Results own their independent allocations after execution-context destruction.

The direct CPU loop polls cancellation at most every 64 samples and at bounded
exact multiplication/division steps. It preserves the caller's floating
environment and supports prevalidated signed, zero and unaligned input strides.
Contiguous spans may use NEON or runtime-checked AVX2. Eligible contiguous
Float32-to-UInt8 tiles can use SME when preparation binds the required hardware
capabilities and 64-byte streaming vector length. SME admission chooses its
proven numeric domain and uses the fixed invocation cancellation callback;
remaining lanes use the same exact scalar semantics. No GPU backend is provided.

| Failure | Phase | Status / reason |
| --- | --- | --- |
| Malformed parameters, malformed bound encoding or table/axis structure, unsupported modes | Static preparation | InvalidArgument / InvalidDomain |
| Unsupported dtype/rank/count or malformed fixed invocation signature | Static preparation / kernel preparation | TypeMismatch |
| Forced view with a nonidentity map | Static preparation | InvalidArgument / InvalidDomain |
| Nonempty forced view with an identity map | Invocation | InvalidArgument / ViewUnavailable |
| Resource admission, cancellation, unavailable backend or producer failure | Kernel/executor phase | Preserve the operational status |

## Acceptance and implementation requirements

The reusable [Fraction oracle](../../../../oracle/ops/format_color/fmt06/reference.py)
rounds directly to each target format; the [C++ references](../../../../oracle/ops/format_color/fmt06/reference.hpp)
cover the common Float32/Int64-to-UInt8 paths. Use independent exact rational
references for all 49 dtype pairs, including
Int64 limits, extrapolation, reversed maps, large cancelling terms, subnormal
rounding, per-channel tables and endpoint signs. Reject cases must satisfy the
caller preconditions; clip cases include finite rounded overflow. Floating
specials and identity use explicit bit-pattern checks.

Exercise complete and selected output tiles, differing source grids, batch and
spatial metadata, signed/zero/unaligned input strides, Empty/view behavior,
malformed static/kernel signatures, cancellation, scratch bounds, floating
environment restoration, concurrent Plan reuse and retained Result owners.
Temporary builtin numeric and public execution checkers are removed after the
family is validated. Keep independent oracles. Native SME dispatch requires
direct hardware evidence; CPU output agreement alone proves no dispatch or
performance claim.
