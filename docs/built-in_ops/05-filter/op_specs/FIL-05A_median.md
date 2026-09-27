---
spec_schema_version: 1
id: FIL-05A
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
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

Inherit [FIL-05](FIL-05_contract.md), [FILTER common contract](FILTER_common_contract.md), [NUM numeric and precision contract](../../01-numeric/op_specs/NUM_common_contract.md), and [FMT metadata and straight-color contract](../../02-format-color/op_specs/FMT_common_contract.md). Together, the family and member specifications define the target. Do not override RN stages, underflow behavior, or accelerated error bounds.

## Ports, shape, and semantics

Inputs are `input` and binary UInt8 `footprint` with shape `[Kh,Kw]`; each footprint entry is 0 or 1 and at least one entry is 1. Output `output` has the input shape.

## Explicit parameters and legal domain

Require axes, anchor, boundary, `cval`, `even`, and `empty`. `even` is `lower`, `upper`, or `mean`. `empty` is `copy_center` or `error`; an empty support is possible only with truncation. The UInt8 footprint has shape `[Kh,Kw]`, entries 0/1, and at least one 1. No constructor defaults.

NUM special-value, floating-point overflow, signed-zero, payload, rounding, and precision behavior follows the corresponding operation and [FILTER common contract](FILTER_common_contract.md). Do not reject NaN/Inf under a family-wide rule or convert ordinary floating-point overflow into failure. Parameter-domain, structural, integer-overflow, resource, cancellation, and upstream errors retain their own contracts. Every parameter is explicit in the node; constructors have no implicit defaults. Interpret floating-point parameters as their stored values.

## Mathematical reference and rounding

Sort the valid footprint samples in numeric ascending order. Preserve stable row-major tap order among equal values, so selected signed-zero bits are retained. For odd N select the middle sample. For even N, `lower` selects index N/2−1; `upper` selects N/2; `mean` returns `RN_t((a+b)/2)`. NaN/Inf ordering, if accepted by the corresponding NUM ordering operation, follows that operation’s exact ordering, propagation, and error rules; do not add a FILTER-only finite-value rejection.

The numeric class and E/B/S meanings are defined in [FILTER numeric reference](FILTER_numeric_reference.md). Retain every declared rounding stage; helper calls do not introduce extra intermediate rounding unless the member says so. Signed zero, discrete choices, gradual underflow, and output range follow the member and NUM contracts.

Demand is stated for each requested output. Exact demand is the actual support after mapping coordinates through the boundary rule; do not substitute a larger rectangle or Whole read for convenience. Kernel, footprint, masks, and static selection parameters are control/validation inputs as specified below. Boundaries are relative to the complete logical image, never to an ROI or tile edge. Empty demand does not read payload, and any selection condition retains the required control dirty witness. Output descriptors derive from static parameters and input descriptors, not sample values.

Validate the complete footprint as control data and read every participating sample required by the NUM ordering operation. Do not validate only the selected result. The reference complexity is O(PCA log A); selection is allowed only if it preserves the same tie and ordering rules. Do not use a histogram shortcut for unrestricted floating-point ranges.

Complexity uses P=HW samples, C independent planes, A taps, and T iterations where applicable; arbitrary-precision limb cost is additional and is not a measured benchmark. Follow FILTER admission, work/memory/stage accounting, cancellation, failure, and owner-lifetime rules. Failure publishes no partial Value or CompleteBundle. Do not spill implicitly, lower precision silently, or fall back to retired implementations.

## Oracle, fixtures, and acceptance

For `[1,2,9,10]`, lower returns 2, upper 9, and mean returns 5.5. Check stable signed zero. SciPy's upper-median behavior is not an oracle for the mean policy.

The linked reference function is a small mathematical oracle, not a production kernel or a complete port-schema/preflight simulator. ExactRational uses exact rational expressions followed by IEEE rounding. For transcendental functions, only a DirectedMPFR interval that uniquely determines the result is a strict golden; otherwise report Inconclusive. Fixture identifiers describe reference-case scope only and make no claim about runtime implementation, full rank/batch behavior, metadata, ROI/dirty behavior, budgets, cancellation, or ownership. See the [runtime acceptance protocol](FILTER_oracle_protocol.md).

See the member's linked reference source and oracle README.

Fixture coverage identifiers: `median_mean`, `median_even_lower`, `median_even_higher`, `median_even_nearest_even`, `median_even_midpoint`, `rank_signed_zero_copy`, `rank_nan_domain`. This catalog records fixture scope only; it makes no current pass claim.

Reference callable: `percentile(image,footprint,anchor,*,q=Q(1,2),interpolation='linear',boundary='reflect_half',cval=0,empty='error',dtype='float64')` in [spatial.py](../../../../oracle/ops/filter/oracles/spatial.py). Select `q=Q(1,2)` and the member's explicit even policy for median behavior. These oracle helper defaults do not define graph-node constructors; every node parameter remains explicitly required.

## Backend and registration

The proposed keys are `filter.median_strict`, `filter.median_accelerated_apple_silicon`, `filter.median_accelerated_x86_64`. They remain unregistered and unimplemented. Accelerated variants must satisfy NUM's final FP32-scaled four-ULP requirement and fallback rules without accumulating a separate budget per tap, axis, stage, or iteration. Threshold, ordering, boundary, copy, and output-support choices that must be exact cannot change approximately. Float64 accelerated paths retain Float64 input/output and exponent range; they do not first convert to Float32. Performance and CPU/ISA differential acceptance are not established here.

## Conceptual DAG (not an existing API)

`static parameters + descriptors -> preflight/demand -> declared data and control -> exact expression and declared rounding stages -> requested owned output/result`

This sketch introduces no implicit color conversion, alpha premultiplication, frequency correction, or zero-fill for missing data. Whole and staged-buffer requirements are those declared by the member and family.

## Sources and open items

[S04](../research-sources.md#s04).
