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

The registry provides one CPU key, `numeric.convert_format_strict`. It accepts one single-tensor Result with no fields and publishes one `values` output Result containing one tensor. It supports all 49 source/target pairs among UInt8, UInt16, Int8, Int16, Int64, Float32 and Float64. There are no separate accelerated keys; results are identical on every CPU path.

The operation preserves Result schema identity, tensor key, logical shape and batch axes. It updates the tensor dtype and `atomic_trailing_axes`. The full sample rank including batch and cell axes is at most 8, and the full sample count is at most 2^40.

Inherits [FMT-common](FMT_common_contract.md) and the NUM numerical baseline. The single member is [A convert numeric format](FMT-06A_convert_numeric_format.md). The default conversion rescales dtype-selected intervals; explicit `rescale=false` requests a pure numerical cast. No bit reinterpretation occurs.

## Purpose and scope

UInt8 255 -> Float32 defaults to 1, while disabling scaling yields 255. Neither mode changes gamma/transfer, color model, primaries, alpha association, pixel coordinates, file packing or byte order. Target dtype applies to the whole tensor; logical shape and channel order are unchanged. Different channel dtypes require separate tensors. A complete image remains straight, with any alpha referenced inside the same tensor.

Effective bit-depth rules, legal code sets and encoding/decoding validation are allocated to future FMT-06 members. Dither and halftone belong to GRD-29/30, and opaque-alpha generation is specified in FMT-05B. FMT-07 is retired.

| Topic | Selected behavior |
| --- | --- |
| Family | Numerical dtype plus interval conversion; no bit reinterpretation. |
| Target dtype | One static dtype for the whole tensor; preserve shape/axes/channel order. |
| Scaling | Enabled by default with dtype-selected intervals; explicit disable gives pure cast and forbids range parameters. |
| Intervals | Shared pairs or complete static per-channel tables with an explicit channel axis; independent source/target defaults or overrides. |
| Bounds | Finite endpoints; source lower<upper; target endpoints differ and may be reversed. |
| Input outside source interval | Extrapolate; no clipping to interval endpoints. |
| Overflow | Round first; reject requires representable rounded results as a caller precondition; clip clamps to destination dtype endpoints. |
| Nonfinite source | Integer targets require finite source samples under both overflow modes; floating targets accept mapped Inf and deterministic NaN propagation. |
| NaN width conversion | Preserve sign/high payload bits, widen with low zeros, narrow by discarding low bits, quiet nonidentity conversion. |
| Identity | Same dtype plus static identity map copies exact bits, including signaling NaN and signed zero. |
| Metadata | Preserve structural schema identity, tensor key, shape, batch axes and spatial topology; update dtype and clear atomic trailing axes. |
| Layout | auto and materialize write independent dense output tiles, including identity; nonempty view is unavailable. |

## Interface

All 49 source/target pairs among UInt8, UInt16, Int8, Int16, Int64, Float32 and Float64 are supported. Float16/Int32/UInt32/UInt64 are outside this member. No mixed-channel dtype, runtime-dependent dtype or implicit promotion is supported. Generic tensors and planar images share the numeric formula.

| Dtype | Default source / target interval |
| --- | --- |
| UInt8 | [0,255] |
| UInt16 | [0,65535] |
| Int8 | [-128,127] |
| Int16 | [-32768,32767] |
| Int64 | [-2^63,2^63-1] |
| Float32 / Float64 | [0,1] |

Defaults come from dtype, not observed extrema or model names. Int8 zero thus maps to 128/255 for floating output; signed zero is not a special range anchor. Exact integer limits are used, including Int64 maximum, never rounded Float64 limits.

| Parameter | Definition / authoring default |
| --- | --- |
| dtype | Required target type, using the seven lowercase names corresponding to the table. |
| rescale | Bool, true by default. False rejects explicit range fields/axis tables. |
| source_range / target_range | Optional typed endpoint pair or complete per-channel pair table; omission independently selects that side's dtype default. |
| axis | Required nonnegative Int64 cell axis when a range has more than one pair; excludes the Result batch prefix. A shared one-pair range needs no axis. Pure cast rejects axis. |
| rounding | ties_even only; other modes require explicit preceding NUM operations. |
| overflow | reject by default, or clip. No wrap mode. |
| layout | auto by default; view or materialize. |

An endpoint is an exact finite typed numeric constant, including the represented value of a floating literal. Typed endpoint tables retain exact integer and floating-bit representations, not untyped Double values. Bounds are static, not per-pixel inputs or data-dependent statistics. Table length equals the channel extent, with no repetition, sparse guessing or role-based matching. A shared source with tabulated target, or the reverse, is allowed. Component inputs use shared intervals. Every table entry is validated even if its channel samples are unrequested. Source endpoints increase; target endpoints may decrease but cannot coincide. Integer endpoint zero has no sign, including `i:-0`; floating endpoint tokens retain their explicit zero sign. Signed-zero-equal endpoints are equal. A constant target belongs to NUM-06B/constant operations, not this conversion.

## Mathematics

For finite exact source value x and resolved bounds s0<s1 and t0!=t1:

```
rescale=true:   v = t0 + (x-s0)*(t1-t0)/(s1-s0)
rescale=false:  v = x
result:        round v to destination, then apply the selected overflow policy
```

The entire mathematical expression is rounded directly to the target once. Integer source and endpoint arithmetic is exact. No intermediate Float32/Float64 conversion defines the reference, and intermediate machine overflow is not an output overflow when exact v is finite.

For integer output, v is rounded to an unbounded mathematical integer using nearest/ties-even, then tested against the target integer bounds. With reject, the caller guarantees the rounded result is in range. With clip, the result is clamped to the corresponding exact bound. There is no wrap. Pure cast -0.5 -> UInt8 succeeds as 0; 255.5 rounds to 256, which violates the reject precondition and clips to 255.

For floating output, v is correctly rounded to the target format with gradual underflow. Finite results succeed, including inexact results and sources just outside the finite target interval that round back to a finite value. A rounded signed infinity from finite v violates the reject precondition; clip returns signed maximum finite. Floating underflow remains successful.

In rescale mode, exact input source endpoints select their corresponding target endpoint values. Endpoint floating zero signs are retained where conversion permits. Ordinary exact-zero affine results follow NUM's +0 rule; pure floating casts preserve source zero sign. Identity paths below take precedence over remap endpoint arithmetic. All static validity checks apply, including for identity requests.

A same-dtype disabled conversion, or same-dtype pair with equal corresponding source/target bounds, is a bit copy. Endpoint zero signs agree for a same-interval identity; differing explicit zero signs require the remap endpoint rule. Identity never depends on observed sample values.

## Nonfinite samples

If the destination is integer, the caller supplies finite original source samples under both reject and clip. Clip handles finite-value overflow only. For floating output, source infinity follows the sign of the exact affine slope, or preserves sign with scaling disabled; it remains infinity. Nonidentity source NaN propagates under the mapping below.

Binary32 and binary64 have 22 and 51 payload bits excluding the quiet bit. NaN sign is preserved, payload bits are left-aligned to the destination field (shift left by 29 on widening, right by 29 on narrowing), and the target quiet bit is set. Same-width nonidentity mapping preserves payload/sign and sets the quiet bit. Discarded payload bits are not rounded; if all retained payload bits vanish, the set quiet bit still produces NaN. Same-dtype static identity copies every bit, including signaling NaN, without quieting.

Integer outputs, discrete choices, endpoint/copy rules, NaN payloads and clip and nonfinite result rules are identical across implementations.

## Behavior

The operation accepts no description or metadata-mode parameter. Source and target ranges come solely from explicit range parameters or the dtype defaults. A caller treating Float32 samples as 0..255 codes supplies `source_range=[0,255]`; omitting it selects the Float32 default [0,1].

The result carries structural schema and converted samples, no updated encoding, decoder, model, alpha association or sampling description. If a source decoder is D and the conversion map is invertible f, D composed with inverse(f) describes the ideal output encoding; rounding and clipping can lose information, so exact recovery is not promised. A pure float cast does not decode integer-like alpha codes into normalized coverage.

Each output sample depends only on the input sample at the same coordinate. The numerical contract requires finite original sources for integer targets and representable correctly rounded results under `overflow=reject`, at every evaluated output coordinate. Inputs need not lie inside the declared source interval. Floating NaN and infinity inputs retain their defined floating-output behavior. Violating a premise has unspecified numerical behavior.

Auto and materialize both write dense output tiles; same-dtype identity copies exact bits. View for a nonidentity declared map fails during static preparation. View for an identity map fails `InvalidArgument / ViewUnavailable` on nonempty execution. Empty demand has no payload; all static checks still apply.

| Failure | Phase | Status / reason |
| --- | --- | --- |
| Malformed parameters, malformed bound encoding or table/axis structure, unsupported modes | Static preparation | InvalidArgument / InvalidDomain |
| Unsupported dtype/rank/count | Static preparation | TypeMismatch |
| Forced view with a nonidentity map | Static preparation | InvalidArgument / InvalidDomain |
| Nonempty forced view with an identity map | Invocation | InvalidArgument / ViewUnavailable |

## Acceptance

Exact rational references for all 49 dtype pairs, including Int64 limits, extrapolation, reversed maps, large cancelling terms, subnormal rounding, per-channel tables and endpoint signs. Reject cases satisfy the caller preconditions; clip cases include finite rounded overflow. Floating specials and identity use explicit bit-pattern checks.
