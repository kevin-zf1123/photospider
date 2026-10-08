---
spec_schema_version: 1
id: FIL-04A
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
parent_id: FIL-04
function: gaussian_coefficients
proposed_operation_keys:
- filter.gaussian_coefficients_strict
- filter.gaussian_coefficients_accelerated_apple_silicon
- filter.gaussian_coefficients_accelerated_x86_64
numeric_reference: B
oracle_scope: mathematical_reference
research_sources:
- S03
- S25
---
# FIL-04A: gaussian_coefficients

Generate baked64 one-dimensional Gaussian coefficients.

Inherit [FIL-04](FIL-04_contract.md), [FILTER common contract](FILTER_common_contract.md), [NUM numeric and precision contract](../../01-numeric/op_specs/NUM_common_contract.md), and [FMT metadata and straight-color contract](../../02-format-color/op_specs/FMT_common_contract.md). Together, the family and member specifications define the target. Do not override RN stages, underflow behavior, or accelerated error bounds.

## Ports, shape, and semantics

There is no dynamic data input. Outputs `kernel_x` and `kernel_y` are Float64 vectors of lengths `2*radius_x+1` and `2*radius_y+1`.

## Explicit parameters and legal domain

Require finite `sigma_x` and `sigma_y` with values ≥0, integer `radius_x` and `radius_y` with values ≥0, and the output axes. On any axis where sigma is zero, radius must be zero. A positive sigma may use radius zero, producing a one-tap discrete kernel. All are explicit; constructors do not infer radius or provide defaults.

NUM special-value, floating-point overflow, signed-zero, payload, rounding, and precision behavior follows the corresponding operation and [FILTER common contract](FILTER_common_contract.md). Do not reject NaN/Inf under a family-wide rule or convert ordinary floating-point overflow into failure. Parameter-domain, structural, integer-overflow, resource, cancellation, and upstream errors retain their own contracts. Every parameter is explicit in the node; constructors have no implicit defaults. Interpret floating-point parameters as their stored values.

## Mathematical reference and rounding

For each axis, `g_s[j]=RN64(exp(-j²/(2*s²)))` for `j=-r…r`; when `s=0`, the kernel is `[1]`. The center is exactly 1. Mirrored positions reuse identical bits. Return unnormalized coefficients; do not form and re-round the outer product of two already rounded kernels.

The numeric class and E/B/S meanings are defined in [FILTER numeric reference](FILTER_numeric_reference.md). Retain every declared rounding stage; helper calls do not introduce extra intermediate rounding unless the member says so. Signed zero, discrete choices, gradual underflow, and output range follow the member and NUM contracts.

Demand is stated for each requested output. Exact demand is the actual support after mapping coordinates through the boundary rule; do not substitute a larger rectangle or Whole read for convenience. Kernel, footprint, masks, and static selection parameters are control/validation inputs as specified below. Boundaries are relative to the complete logical image, never to an ROI or tile edge. Empty demand does not read payload, and any selection condition retains the required control dirty witness. Output descriptors derive from static parameters and input descriptors, not sample values.

There is no source-data input; descriptors and static parameters determine every output. Each coefficient depends on its axis sigma and radius. Work is O(rx+ry). Evaluate exp with a directed interval until RN64 is uniquely determined; tiny coefficients may underflow to positive zero.

Complexity uses P=HW samples, C independent planes, A taps, and T iterations where applicable; arbitrary-precision limb cost is additional and is not a measured benchmark. Follow FILTER admission, work/memory/stage accounting, cancellation, failure, and owner-lifetime rules. Failure publishes no partial Value or CompleteBundle. Do not spill implicitly, lower precision silently, or fall back to retired implementations.

## Oracle, fixtures, and acceptance

For sigma zero and radius zero, return `[1]`. For sigma 1 and radius 1, return `[RN64(exp(-1/2)),1,the same bits as the first value]`. Large radii may include weights that round to zero. Negative sigma and a nonzero radius paired with zero sigma are invalid.

The linked reference function is a small mathematical oracle, not a production kernel or a complete port-schema/preflight simulator. ExactRational uses exact rational expressions followed by IEEE rounding. For transcendental functions, only a DirectedMPFR interval that uniquely determines the result is a strict golden; otherwise report Inconclusive. Fixture identifiers describe reference-case scope only and make no claim about runtime implementation, full rank/batch behavior, metadata, ROI/dirty behavior, budgets, cancellation, or ownership. See the [runtime acceptance protocol](FILTER_oracle_protocol.md).

The reference helper is `gaussian_kernel(sigma,radius)` in [transcend.py](../../../../oracle/ops/filter/oracles/transcend.py). See also the [oracle README](../../../../oracle/ops/filter/README.md).

Fixture coverage identifiers: `gaussian_coefficients`, `gaussian_zero_sigma`, `gaussian_zero_sigma_nonzero_radius`.

## Backend and registration

The proposed keys are `filter.gaussian_coefficients_strict`, `filter.gaussian_coefficients_accelerated_apple_silicon`, `filter.gaussian_coefficients_accelerated_x86_64`. They remain unregistered and unimplemented. Accelerated variants must satisfy NUM's final FP32-scaled four-ULP requirement and fallback rules without accumulating a separate budget per tap, axis, stage, or iteration. Threshold, ordering, boundary, copy, and output-support choices that must be exact cannot change approximately. Float64 accelerated paths retain Float64 input/output and exponent range; they do not first convert to Float32. Performance and CPU/ISA differential acceptance are not established here.

## Conceptual DAG (not an existing API)

`static parameters + descriptors -> preflight/demand -> declared data and control -> exact expression and declared rounding stages -> requested owned output/result`

This sketch introduces no implicit color conversion, alpha premultiplication, frequency correction, or zero-fill for missing data. Whole and staged-buffer requirements are those declared by the member and family.

## Sources and open items

[S03](../research-sources.md#s03), [S25](../research-sources.md#s25).
