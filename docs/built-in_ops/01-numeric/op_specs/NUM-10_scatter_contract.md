---
spec_schema_version: 1
id: NUM-10-scatter
parent_id: NUM-10
kind: shared_operator_contract
category: 01-numeric
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_manual_acceptance
---

# NUM-10: scatter into a base tensor

The strict key follows its exact numeric reference. Accelerated floating
results follow the shared [final FP32 four-ULP contract](NUM_accelerated_contract.md)
where arithmetic applies; raw copies and discrete results remain exact.

Inherit the [NUM baseline](NUM_common_contract.md) for specification status,
registration, shared execution and acceptance requirements. The rules below
specify the NUM-10 behavior.

The four operations `scatter_replace`, `scatter_sum`, `scatter_minimum` and
`scatter_maximum` each have strict and two CPU accelerated formal keys. Their
public ports use `Result`. Each input `Result` contains exactly one tensor
member, whose key is unrestricted. The kernel reads that member's complete
`sample_shape()`, including any batch prefix. Shapes have rank 1..8 and at most
2^40 elements. The numeric schema does not require frame or layer facets.

Inputs are ordered `base`, `indices`, `updates`. `indices` is a dynamic Int64
rank-one tensor `[M]`; `axis` is a required static Int64 parameter in
`[0, rank)`. `updates` has the base dtype and rank, the same non-axis extents,
and extent `M` on the selected axis. Dtypes are UInt8, Int64, Float32 and
Float64. The output port `values` is a Result with schema `photospider.tensor`
and one `samples` member. It contains the full base shape as ordinary axes,
with facets and batch topology removed. The operation never mutates `base`.

For update position `j`, `indices[j]` selects the base coordinate along `axis`.
Repeated targets are valid. `scatter_replace` selects the greatest matching
`j`. The aggregate operations include `base` first and then matching updates in
increasing `j`; that order defines exceptional-value priority, not a sequence of
rounded additions. A coordinate with no matching update copies the base bits.

Every nonempty Whole request asks for complete data, validation and descriptor
support (role 13) from all three inputs. It validates every index against the
base axis and computes the complete output before consumer projection. Thus an
invalid index or an upstream/typed failure outside the requested output region
can fail the run. Empty demand requests no input payload and performs no sample
arithmetic. Edits to any active input invalidate the complete output.

## Types, storage and resource contract

Outputs use complete packed storage. Immutable inputs may have negative or zero
strides and unaligned elements. No writable alias is published. The stable
eight-pass radix grouping costs O(M); output coordinates locate contributors by
binary search in O(log M), followed by the required arithmetic. The plan and
sorting vector each hold M pairs of two uint64 values, for a peak of 32*M
metadata bytes; the sorting vector is released after grouping. The exact sum
accumulator and fixed kernel state are admitted through the host resource
ledger. Coordinate vectors are bounded by rank eight.

The kernel charges work during index scan, sorting, contributor lookup and
output writing. Cancellation, work or capacity failure releases unpublished
output and temporary state. A failure does not certify a new output region.

## Errors and numerical rules

Preflight rejects unsupported dtype, wrong rank or extents, wrong index dtype or
rank, invalid axis and logical element counts above 2^40. Runtime invalid
indices return `InvalidArgument`, `FailureReason::InvalidDomain` and the
`IndexOutOfBounds` diagnostic with position, value and destination extent.
Integer sum overflow returns `OperationFailed` with
`FailureReason::ArithmeticOverflow`, attributed to the complete output
coordinate as Domain/Run. Other source, typed-validation, resource and
cancellation failures keep their established categories.

For a no-hit coordinate, every operation copies base bits exactly, including a
signaling NaN. Replacement also copies the chosen update bits without arithmetic.
For a hit in an aggregate operation, values are classified in base-then-update
order. The first NaN wins; its sign and payload are preserved and its quiet bit
is set before generated exceptional results are considered.

For sum without NaNs, simultaneous positive and negative infinities produce the
fixed positive quiet NaN. Otherwise infinity determines its signed result.
Finite contributors are summed exactly and rounded once at the end for floating
output; integer output is range-checked only after exact accumulation. Exact zero
is negative zero only when every contributor is negative zero. Nonzero exact
underflow retains its sign. Strict results are reproducible; accelerated
floating results follow the shared FP32-scaled bound.

Minimum and maximum use the NUM-05 numeric ordering. For mixed signed zeros,
minimum selects -0 and maximum selects +0; same-sign zeros retain their sign.
Integer comparisons remain integer comparisons. These rules apply only when an
update hits the coordinate.

## Individual specifications

| Spec | Duplicate target behavior |
| --- | --- |
| [NUM-10C scatter_replace](NUM-10C_scatter_replace.md) | Last matching update wins |
| [NUM-10D scatter_sum](NUM-10D_scatter_sum.md) | Exact sum including base |
| [NUM-10E scatter_minimum](NUM-10E_scatter_minimum.md) | Minimum including base, NaN propagating |
| [NUM-10F scatter_maximum](NUM-10F_scatter_maximum.md) | Maximum including base, NaN propagating |

The shared execution and current behavior evidence are summarized in
[NUM-10 Whole execution](../indexing-whole.md).
