---
spec_schema_version: 1
id: NUM-11
kind: shared_operator_contract
category: 01-numeric
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_manual_acceptance
repository_branch: ops-specs
repository_commit: current working tree
---

# NUM-11: reductions

The seven operations `reduce_sum`, `reduce_minimum`, `reduce_maximum`, `reduce_mean`, `reduce_count`, `reduce_variance` and `reduce_std` each have strict, Apple Silicon CPU accelerated and x86-64 CPU accelerated formal keys. The strict key follows its exact reference. Accelerated floating arithmetic follows the shared [final FP32 four-ULP contract](NUM_accelerated_contract.md), including its range and fallback rules. Discrete results, selections, copies and special values remain exact. Inherit the [NUM baseline](NUM_common_contract.md) for status, registration, shared execution and acceptance requirements; explicit rules in this contract and member specs take precedence.

Each input port accepts a Result containing exactly one tensor member at any member key. The operation reads its complete `sample_shape()`, including batch axes. Input rank is 1..8, extents are positive, and logical element count is at most 2^40. Each operation publishes `values`, a Result with schema `photospider.tensor` and one `samples` tensor. The output keeps input rank, sets every reduced axis extent to one, uses ordinary axes, and drops facets and batch topology. A full reduction therefore produces rank-preserving all-one extents, not a rank-zero scalar. No reduction group is empty under these shape rules.

`axes` is a required static String containing a nonempty canonical comma-separated list of distinct nonnegative decimal axis indices within rank. Entries use ASCII digits without whitespace, plus signs or leading zeros; constructors sort axes in increasing order. Axis order in a direct node does not change grouping. There is no `keepdims` parameter. Each output coordinate groups every input coordinate that matches its nonreduced axes and spans the full extent of each reduced axis. All operations use complete schema validation before execution.

## Dtype and mathematical domains

`reduce_sum` supports UInt8, Int64, Float32 and Float64 input. Integer inputs produce UInt8 or Int64 output; floating inputs produce Float32 or Float64 output. Constructors default to Int64 for integer input and Float64 for floating input. Sum uses exact total arithmetic and performs only one final integer range check or correctly rounded floating conversion.

`reduce_minimum` and `reduce_maximum` preserve input dtype. `reduce_mean`, `reduce_variance` and `reduce_std` accept all four input dtypes and produce Float32 or Float64 output, defaulting to Float64 in authoring helpers. Integer source values participate exactly, without preliminary conversion to float. `reduce_count` produces Int64 and derives each group count from the reduced extents alone.

For sum, minimum, maximum and mean, the first NaN in logical row-major order within each group wins, preserving its sign and payload while setting the quiet bit. This order does not depend on axes-string order, physical strides, block size or execution scheduling. When source and destination float types match, the source payload bits are retained. For Float32-to-Float64 conversion, shift the payload (excluding the quiet bit) left by 29 bits. For Float64-to-Float32, shift it right by 29 bits, setting destination payload bit zero if a nonzero source payload would otherwise disappear. Newly generated NaNs use positive quiet patterns `0x7fc00000` and `0x7ff8000000000000` for Float32 and Float64.

Sum handles opposite-signed infinities without a NaN input as the fixed positive quiet NaN; otherwise infinity determines the signed result. Finite terms are summed exactly. Exact zero is -0 only when every contributor is -0; otherwise it is +0. Integer overflow is checked only on the final exact total; floating overflow returns signed infinity. Mean divides the exact total by positive group size and rounds once. Its nonfinite rules match sum.

Minimum and maximum use numeric extended ordering. Minimum of mixed signed zeros is -0, maximum is +0, and same-sign zeros retain their sign. Int64 comparisons remain integer comparisons. Singleton groups are still numerical reductions: NaNs are quieted rather than copied as raw bits.

For `reduce_variance`, the static `ddof` is a nonnegative Int64 with `ddof < N`, where N is the group size determined by shape and axes. The helper defaults it to zero; direct nodes provide it explicitly. For finite samples, compute the exact rational

```text
V = sum((x - mean)^2) / (N - ddof)
  = (N * sum(x^2) - sum(x)^2) / (N * (N - ddof))
```

and round V once to the selected floating type. Do not round the mean, squares or partial sums. Integer inputs enter this formula exactly. Its exact numerator is nonnegative; clamping a negative approximate variance is not a substitute for the formula. For `reduce_std`, compute the exact same V and return `RN_dtype(sqrt(V))` without first rounding V. Both operations propagate the first NaN using the shared payload conversion. If there is an infinity and no NaN, return the fixed positive quiet NaN. A finite constant group has variance and standard deviation +0. Finite positive results may round to +Inf or +0.

## Whole execution and count's metadata path

The six numeric reducers run as Whole tensor programs. Static specialization validates the complete Result schema and parameters. For nonempty demand, each requests the complete input with Data, Validation and Descriptor roles (role 13), validates the typed payload, and reads through the authorized Result windows. It computes all keepdims groups and publishes the complete dense output before consumer projection; it does not first collect or pack the complete input. Thus a typed-validation failure or arithmetic overflow in an unrequested group fails the selected output. Empty demand reads no payload or performs group arithmetic. Any active source edit dirties the complete output.

`reduce_count` specializes with an empty runtime input projection. It validates the complete input schema and axes, but requests no runtime input, creates no source observation or association, and does not schedule a source producer for sample values. Its descriptor relation is a known empty witness. It computes the product of reduced extents with O(rank) work, allocates one 8-byte Int64 owner, and publishes the keepdims result through zero strides. Source-byte edits do not invalidate the count; schema shape/type and axes determine it. Cancellation, work and resource errors keep their original host status.

Numeric reducers own the complete dense output and hold authorized input read windows while processing it. Exact aggregate or moments state is fixed workspace, admitted through the phase allocator. Coordinates are bounded by rank eight. Work covers the full input and exact arithmetic; cancellation is checked while reading and accumulating and before publication. A failed run exposes no partial output. Count uses metadata, its 8-byte backing and a zero-stride immutable mapping instead of a dense output allocation. Published owners survive context destruction until released by their final owner; these managed byte counts are not an RSS bound.

## Errors and acceptance evidence

Compile/preflight rejects malformed or empty axes, duplicate/out-of-rank axes, unsupported dtype or shape, invalid numeric parameters and input sizes above 2^40. Invalid `ddof` reports `InvalidArgument` / `InvalidDomain` before sample reads. Integer final overflow reports `OperationFailed` / `ArithmeticOverflow`, attributed to the failing linear output group as Domain/Run. Floating numeric outcomes such as infinity or generated NaN are results, not Status failures. Upstream, typed-validation, resource and cancellation failures retain their original categories. No partial failed output is published.

The seven member specifications define per-operation outputs, formulas and acceptance cases. Current public integration coverage and its limits are summarized in [NUM-11 Whole execution](../reductions-whole.md). The specifications remain Proposed.

## Individual specifications

| Spec | Output dtype | Formula |
| --- | --- | --- |
| [NUM-11A reduce_sum](NUM-11A_reduce_sum.md) | Integer or floating domain matching input; default Int64/Float64 | Exact total, final range check or rounding |
| [NUM-11B reduce_minimum](NUM-11B_reduce_minimum.md) | Input dtype | Numeric selection |
| [NUM-11C reduce_maximum](NUM-11C_reduce_maximum.md) | Input dtype | Numeric selection |
| [NUM-11D reduce_mean](NUM-11D_reduce_mean.md) | Float32/Float64; default Float64 | Exact sum/count, final rounding |
| [NUM-11E reduce_count](NUM-11E_reduce_count.md) | Int64 | Metadata product |
| [NUM-11F reduce_variance](NUM-11F_reduce_variance.md) | Float32/Float64; default Float64 | Exact moments, final rounding |
| [NUM-11G reduce_std](NUM-11G_reduce_std.md) | Float32/Float64; default Float64 | Exact moments and square root, one final rounding |
