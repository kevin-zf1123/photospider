---
spec_schema_version: 1
id: NUM-11
kind: shared_operator_contract
category: 01-numeric
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_manual_acceptance
---

# NUM-11: reductions

The seven operations `reduce_sum`, `reduce_minimum`, `reduce_maximum`, `reduce_mean`, `reduce_count`, `reduce_variance` and `reduce_std` each have strict, Apple Silicon CPU accelerated and x86-64 CPU accelerated formal keys. The strict key follows its exact reference. Accelerated floating arithmetic follows the shared [final FP32 four-ULP contract](NUM_accelerated_contract.md), including its range and fallback rules. Discrete results, selections, copies and special values remain exact. Inherit the [NUM baseline](NUM_common_contract.md) for status, registration, shared execution and acceptance requirements; explicit rules in this contract and member specs take precedence.

Each input port accepts a Result containing exactly one tensor member at any member key. The operation reads its complete `sample_shape()`, including batch axes. Input rank is 1..8, extents are positive, and logical element count is at most 2^40. Each operation publishes `values`, a Result with schema `photospider.tensor` and one `samples` tensor. The output keeps input rank, sets every reduced axis extent to one, uses ordinary axes, and drops batch topology. A full reduction therefore produces rank-preserving all-one extents, not a rank-zero scalar. No reduction group is empty under these shape rules.

`axes` is a required static String containing a nonempty canonical comma-separated list of distinct nonnegative decimal axis indices within rank. Entries use ASCII digits without whitespace, plus signs or leading zeros; constructors sort axes in increasing order. Axis order in a direct node does not change grouping. There is no `keepdims` parameter. Each output coordinate groups every input coordinate that matches its nonreduced axes and spans the full extent of each reduced axis. All operations use complete schema validation before execution.

## Dtype and mathematical domains

`reduce_sum` supports UInt8, Int64, Float32 and Float64 input. Integer inputs produce UInt8 or Int64 output; floating inputs produce Float32 or Float64 output. Constructors default to Int64 for integer input and Float64 for floating input. Sum uses exact total arithmetic and performs one final conversion. The exact integer total must fit its destination as an unchecked caller precondition; floating conversion is correctly rounded.

`reduce_minimum` and `reduce_maximum` preserve input dtype. `reduce_mean`, `reduce_variance` and `reduce_std` accept all four input dtypes and produce Float32 or Float64 output, defaulting to Float64 in authoring helpers. Integer source values participate exactly, without preliminary conversion to float. `reduce_count` produces Int64 and derives each group count from the reduced extents alone.

For sum, minimum, maximum and mean, the first NaN in logical row-major order within each group wins, preserving its sign and payload while setting the quiet bit. This order does not depend on axes-string order, physical strides, block size or execution scheduling. When source and destination float types match, the source payload bits are retained. For Float32-to-Float64 conversion, shift the payload (excluding the quiet bit) left by 29 bits. For Float64-to-Float32, shift it right by 29 bits, setting destination payload bit zero if a nonzero source payload would otherwise disappear. Newly generated NaNs use positive quiet patterns `0x7fc00000` and `0x7ff8000000000000` for Float32 and Float64.

Sum handles opposite-signed infinities without a NaN input as the fixed positive quiet NaN; otherwise infinity determines the signed result. Finite terms are summed exactly. Exact zero is -0 only when every contributor is -0; otherwise it is +0. Integer totals must be representable in the selected destination; floating overflow returns signed infinity. Mean divides the exact total by positive group size and rounds once. Its nonfinite rules match sum.

Minimum and maximum use numeric extended ordering. Minimum of mixed signed zeros is -0, maximum is +0, and same-sign zeros retain their sign. Int64 comparisons remain integer comparisons. Singleton groups are still numerical reductions: NaNs are quieted rather than copied as raw bits.

For `reduce_variance`, the static `ddof` is a nonnegative Int64 with `ddof < N`, where N is the group size determined by shape and axes. The helper defaults it to zero; direct nodes provide it explicitly. For finite samples, compute the exact rational

```text
V = sum((x - mean)^2) / (N - ddof)
  = (N * sum(x^2) - sum(x)^2) / (N * (N - ddof))
```

and round V once to the selected floating type. Do not round the mean, squares or partial sums. Integer inputs enter this formula exactly. Its exact numerator is nonnegative; clamping a negative approximate variance is not a substitute for the formula. For `reduce_std`, compute the exact same V and return `RN_dtype(sqrt(V))` without first rounding V. Both operations propagate the first NaN using the shared payload conversion. If there is an infinity and no NaN, return the fixed positive quiet NaN. A finite constant group has variance and standard deviation +0. Finite positive results may round to +Inf or +0.

## Fixed tasks and metadata-only count

The six numerical reducers lower one ABI 8 CPU task per selected output tile. The task declares every input tile intersecting the Cartesian product of its nonreduced coordinate ranges and the full extents of its reduced axes. It computes only the output groups in that tile, in logical row-major order. Input tile granularity can include additional samples; those samples do not add output groups or change numerical order. Empty selection emits no task. Complete schema and static parameter checks run before lowering; prepare validates every declared read, output view, kernel contract and scratch capacity.

`reduce_count` declares a schema-only input reference with no payload tile or producer dependency. Each selected output tile becomes a task that fills its separate dense Int64 allocation with the static product of reduced extents. It uses zero arithmetic scratch. An upstream sample producer remains elided when no other selected consumer needs its payload. An external input binding still supplies the declared schema; empty payload coverage suffices for count. Schema and parameter changes require a newly compiled Plan.

Numerical tasks borrow fixed input views and use a preallocated `ReductionState` workspace containing exact aggregate or moments arithmetic. Mutable state is private to each invocation; prepared contexts hold immutable metadata and read-slot tables. Coordinates have rank at most eight. Cancellation is queried at each output group and within exact arithmetic. Operational failures discard unpublished output and drain accepted work before releasing owners. Successful output tiles retain independent owners beyond Run/context lifetime. The compiler bounds graph construction, the executor admits block resources and structural work, and exact-arithmetic callbacks query cancellation; they do not impose a per-sample execution-work counter. Managed resource accounting is not an RSS bound.

## Errors and acceptance

Compilation rejects malformed or empty axes, duplicate/out-of-rank axes, unsupported dtype or shape, invalid static parameters and input sizes above 2^40. Invalid `ddof` reports `InvalidArgument` / `InvalidDomain` before sample reads. Integer result representability is an unchecked caller obligation; diagnostics for violating it are unspecified. Floating infinity, NaN and underflow are numerical results. Resource and cancellation failures retain their operational categories; failed Runs publish no final RET.

The seven member specifications define formulas and acceptance cases. Use independent exact rational totals/moments and squared-midpoint square-root rounding to check raw output bits. Cover sparse and empty requests, input tile boundaries, negative strides, batch/atomic geometry, NaN priority and count without upstream payload. The [fixed reduction execution page](../reductions-execution.md) describes the current implementation and validation scope. The specifications remain Proposed.

## Individual specifications

| Spec | Output dtype | Formula |
| --- | --- | --- |
| [NUM-11A reduce_sum](NUM-11A_reduce_sum.md) | Integer or floating domain matching input; default Int64/Float64 | Exact total, final conversion |
| [NUM-11B reduce_minimum](NUM-11B_reduce_minimum.md) | Input dtype | Numeric selection |
| [NUM-11C reduce_maximum](NUM-11C_reduce_maximum.md) | Input dtype | Numeric selection |
| [NUM-11D reduce_mean](NUM-11D_reduce_mean.md) | Float32/Float64; default Float64 | Exact sum/count, final rounding |
| [NUM-11E reduce_count](NUM-11E_reduce_count.md) | Int64 | Metadata product |
| [NUM-11F reduce_variance](NUM-11F_reduce_variance.md) | Float32/Float64; default Float64 | Exact moments, final rounding |
| [NUM-11G reduce_std](NUM-11G_reduce_std.md) | Float32/Float64; default Float64 | Exact moments and square root, one final rounding |
