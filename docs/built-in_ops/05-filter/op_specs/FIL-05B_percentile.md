---
spec_schema_version: 1
id: FIL-05B
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
parent_id: FIL-05
function: percentile
proposed_operation_keys:
- filter.percentile_strict
- filter.percentile_accelerated_apple_silicon
- filter.percentile_accelerated_x86_64
numeric_reference: E
oracle_scope: mathematical_reference
research_sources:
- S04
---
# FIL-05B: percentile

Linear or discrete percentile.

Inherit [FIL-05](FIL-05_contract.md), [FILTER common contract](FILTER_common_contract.md), [NUM numeric and precision contract](../../01-numeric/op_specs/NUM_common_contract.md), and [FMT metadata and straight-color contract](../../02-format-color/op_specs/FMT_common_contract.md). Together, the family and member specifications define the target. Do not override RN stages, underflow behavior, or accelerated error bounds.

## Ports, shape, and semantics

Ports and support match FIL-05A: input field and binary UInt8 footprint produce an output of the input shape.

## Explicit parameters and legal domain

Require the FIL-05A footprint, axes, anchor, boundary, `cval`, and empty policy, plus `q∈[0,1]` and interpolation (`linear`, `lower`, `higher`, `nearest_even`, or `midpoint`). These are static parameters; constructors do not supply defaults.

NUM special-value, floating-point overflow, signed-zero, payload, rounding, and precision behavior follows the corresponding operation and [FILTER common contract](FILTER_common_contract.md). Do not reject NaN/Inf under a family-wide rule or convert ordinary floating-point overflow into failure. Parameter-domain, structural, integer-overflow, resource, cancellation, and upstream errors retain their own contracts. Every parameter is explicit in the node; constructors have no implicit defaults. Interpret floating-point parameters as their stored values.

## Mathematical reference and rounding

Order samples as in FIL-05A. Compute exact `h=(N−1)*q`, `i=floor(h)`, `j=ceil(h)`, `f=h−i`. Linear returns `RN_t((1−f)a_i+f*a_j)`; if f=0, copy the selected sample bits. `lower`/`higher` select i/j. `nearest_even` rounds h to an integer index using ties-to-even. `midpoint` copies when i=j and otherwise returns `RN_t((a_i+a_j)/2)`.

The numeric class and E/B/S meanings are defined in [FILTER numeric reference](FILTER_numeric_reference.md). Retain every declared rounding stage; helper calls do not introduce extra intermediate rounding unless the member says so. Signed zero, discrete choices, gradual underflow, and output range follow the member and NUM contracts.

Demand is stated for each requested output. Exact demand is the actual support after mapping coordinates through the boundary rule; do not substitute a larger rectangle or Whole read for convenience. Kernel, footprint, masks, and static selection parameters are control/validation inputs as specified below. Boundaries are relative to the complete logical image, never to an ROI or tile edge. Empty demand does not read payload, and any selection condition retains the required control dirty witness. Output descriptors derive from static parameters and input descriptors, not sample values.

Demand, footprint validation, and boundary support follow FIL-05A. q is static control. Compute the integer indices from exact q; do not pre-round h to floating point. Do not substitute a different library quantile default.

Complexity uses P=HW samples, C independent planes, A taps, and T iterations where applicable; arbitrary-precision limb cost is additional and is not a measured benchmark. Follow FILTER admission, work/memory/stage accounting, cancellation, failure, and owner-lifetime rules. Failure publishes no partial Value or CompleteBundle. Do not spill implicitly, lower precision silently, or fall back to retired implementations.

## Oracle, fixtures, and acceptance

Check exact endpoints q=0 and q=1. On `[0,10]` at q=1/4, linear interpolation returns 2.5. At q=1/2, nearest-even selects index 0. Include large N and q values near index boundaries to detect double rounding.

The linked reference function is a small mathematical oracle, not a production kernel or a complete port-schema/preflight simulator. ExactRational uses exact rational expressions followed by IEEE rounding. For transcendental functions, only a DirectedMPFR interval that uniquely determines the result is a strict golden; otherwise report Inconclusive. Fixture identifiers describe reference-case scope only and make no claim about runtime implementation, full rank/batch behavior, metadata, ROI/dirty behavior, budgets, cancellation, or ownership. See the [runtime acceptance protocol](FILTER_oracle_protocol.md).

See the member's linked reference source and oracle README.

Fixture coverage identifiers: `percentile_linear_float32`, `percentile_linear_float64`, `median_even_lower`, `median_even_higher`, `median_even_nearest_even`, `median_even_midpoint`, `rank_signed_zero_copy`.

Reference callable: `percentile(image,footprint,anchor,*,q=Q(1,2),interpolation='linear',boundary='reflect_half',cval=0,empty='error',dtype='float64')` in [spatial.py](../../../../oracle/ops/filter/oracles/spatial.py). These oracle helper defaults do not define graph-node constructors; every node parameter remains explicitly required.

## Backend and registration

The proposed keys are `filter.percentile_strict`, `filter.percentile_accelerated_apple_silicon`, `filter.percentile_accelerated_x86_64`. They remain unregistered and unimplemented. Accelerated variants must satisfy NUM's final FP32-scaled four-ULP requirement and fallback rules without accumulating a separate budget per tap, axis, stage, or iteration. Threshold, ordering, boundary, copy, and output-support choices that must be exact cannot change approximately. Float64 accelerated paths retain Float64 input/output and exponent range; they do not first convert to Float32. Performance and CPU/ISA differential acceptance are not established here.

## Conceptual DAG (not an existing API)

`static parameters + descriptors -> preflight/demand -> declared data and control -> exact expression and declared rounding stages -> requested owned output/result`

This sketch introduces no implicit color conversion, alpha premultiplication, frequency correction, or zero-fill for missing data. Whole and staged-buffer requirements are those declared by the member and family.

## Sources and open items

[S04](../research-sources.md#s04).
