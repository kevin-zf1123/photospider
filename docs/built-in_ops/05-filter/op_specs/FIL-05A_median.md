---
spec_schema_version: 1
id: FIL-05A
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
parent_id: FIL-05
function: median
proposed_operation_keys:
- filter.median_strict
- filter.median_accelerated_apple_silicon
- filter.median_accelerated_x86_64
numeric_reference: E
oracle_scope: mathematical_reference
research_sources:
- S04
---
# FIL-05A: median

Median with an explicit even-window policy.

Inherits [FIL-05](FIL-05_contract.md), the [FILTER common contract](FILTER_common_contract.md), [NUM](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT](../../02-format-color/op_specs/FMT_common_contract.md).

## Interface

Operation keys: `filter.median_strict`, `filter.median_accelerated_apple_silicon`, `filter.median_accelerated_x86_64`.

Inputs are `input` and binary UInt8 `footprint` with shape `[Kh,Kw]`; each footprint entry is 0 or 1 and at least one entry is 1. Output `output` has the input shape.

### Parameters

Require axes, anchor, boundary, `cval`, `even`, and `empty`. `even` is `lower`, `upper`, or `mean`. `empty` is `copy_center` or `error`; an empty support is possible only with truncation. The UInt8 footprint has shape `[Kh,Kw]`, entries 0/1, and at least one 1. No constructor defaults.

## Mathematics

Sort the valid footprint samples in numeric ascending order. Preserve stable row-major tap order among equal values, so selected signed-zero bits are retained. For odd N select the middle sample. For even N, `lower` selects index N/2−1; `upper` selects N/2; `mean` returns `RN_t((a+b)/2)`. NaN/Inf ordering, if accepted by the corresponding NUM ordering operation, follows that operation’s exact ordering, propagation, and error rules; do not add a FILTER-only finite-value rejection.

Consume the complete footprint as algorithmic control and every participating sample required by the NUM ordering operation. Footprint encoding and numerical premises are caller obligations.

## Acceptance

For `[1,2,9,10]`, lower returns 2, upper 9, and mean returns 5.5. Check stable signed zero. SciPy's upper-median behavior is not an oracle for the mean policy.

## References

[S04](../research-sources.md#s04).
