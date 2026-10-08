---
spec_schema_version: 1
id: FIL-02B
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
parent_id: FIL-02
function: separable_correlation
proposed_operation_keys:
- filter.separable_correlation_strict
- filter.separable_correlation_accelerated_apple_silicon
- filter.separable_correlation_accelerated_x86_64
numeric_reference: E
oracle_scope: mathematical_reference
research_sources:
- S01
---
# FIL-02B: separable_correlation

Separable two-dimensional correlation.

Inherit [FIL-02](FIL-02_contract.md), [FILTER common contract](FILTER_common_contract.md), [NUM numeric and precision contract](../../01-numeric/op_specs/NUM_common_contract.md), and [FMT metadata and straight-color contract](../../02-format-color/op_specs/FMT_common_contract.md). Together, the family and member specifications define the target. Do not override RN stages, underflow behavior, or accelerated error bounds.

## Ports, shape, and semantics

Inputs are `input`, `kernel_x`, and `kernel_y`; output is `output`. Both kernels are positive-length Float64 vectors. Output shape and global origin follow the explicit `same|full|valid` selection in [boundary geometry](FILTER_boundary_contract.md); only `same` preserves input shape.

## Explicit parameters and legal domain

Both one-dimensional kernels are positive-length Float64 arrays. Require `y_axis`, `x_axis`, both anchors, shape (`same`, `full`, or `valid`), boundary, `cval`, normalization (`none`, `sum`, or `l1`), and finite Float64 bias. Full/valid require constant boundary with `cval=0`. Constructors have no defaults.

NUM special-value, floating-point overflow, signed-zero, payload, rounding, and precision behavior follows the corresponding operation and [FILTER common contract](FILTER_common_contract.md). Do not reject NaN/Inf under a family-wide rule or convert ordinary floating-point overflow into failure. Parameter-domain, structural, integer-overflow, resource, cancellation, and upstream errors retain their own contracts. Every parameter is explicit in the node; constructors have no implicit defaults. Interpret floating-point parameters as their stored values.

## Mathematical reference and rounding

The exact mathematical kernel is `K[j,i]=ky[j]*kx[i]`; it need not be materialized as a Float64 2D array. Apply the direction, denominator, and one final RN_t from [FIL-01B](FIL-01B_correlate2d.md). Sum normalization uses `(Σkx)(Σky)`; l1 uses `(Σ|kx|)(Σ|ky|)`.

The numeric class and E/B/S meanings are defined in [FILTER numeric reference](FILTER_numeric_reference.md). Retain every declared rounding stage; helper calls do not introduce extra intermediate rounding unless the member says so. Signed zero, discrete choices, gradual underflow, and output range follow the member and NUM contracts.

Demand is stated for each requested output. Exact demand is the actual support after mapping coordinates through the boundary rule; do not substitute a larger rectangle or Whole read for convenience. Kernel, footprint, masks, and static selection parameters are control/validation inputs as specified below. Boundaries are relative to the complete logical image, never to an ROI or tile edge. Empty demand does not read payload, and any selection condition retains the required control dirty witness. Output descriptors derive from static parameters and input descriptors, not sample values.

The support is the Cartesian product of the nonzero taps on both axes after boundary mapping. All kernel values are control data. When implementing as horizontal then vertical passes, constant virtual rows must remain equivalent to 2D constant extension; do not treat `cval` as a filtered row value. Complexity is O(PC(Kh+Kw)); retain exact intermediates for strict mode. Ordinary same-dtype two-pass rounding defines a different S operator.

Complexity uses P=HW samples, C independent planes, A taps, and T iterations where applicable; arbitrary-precision limb cost is additional and is not a measured benchmark. Follow FILTER admission, work/memory/stage accounting, cancellation, failure, and owner-lifetime rules. Failure publishes no partial Value or CompleteBundle. Do not spill implicitly, lower precision silently, or fall back to retired implementations.

## Oracle, fixtures, and acceptance

With `kx=[1,2]` and `ky=[1,3]`, compare with direct application of the exact outer-product kernel. Include nonzero constant extension, non-unit normalization, corner samples that detect incorrect two-pass extension, and Float32 cancellation that detects intermediate rounding.

The linked reference function is a small mathematical oracle, not a production kernel or a complete port-schema/preflight simulator. ExactRational uses exact rational expressions followed by IEEE rounding. For transcendental functions, only a DirectedMPFR interval that uniquely determines the result is a strict golden; otherwise report Inconclusive. Fixture identifiers describe reference-case scope only and make no claim about runtime implementation, full rank/batch behavior, metadata, ROI/dirty behavior, budgets, cancellation, or ownership. See the [runtime acceptance protocol](FILTER_oracle_protocol.md).

See the member's linked reference source and oracle README.

Fixture coverage identifiers: `separable_correlate`.

Reference callable: `separable(image,kx,ky,anchor=(0,0),**kwargs)` in [spatial.py](../../../../oracle/ops/filter/oracles/spatial.py). Pass the explicit correlation direction through `kwargs`. These oracle helper defaults do not define graph-node constructors; every node parameter remains explicitly required.

## Backend and registration

The proposed keys are `filter.separable_correlation_strict`, `filter.separable_correlation_accelerated_apple_silicon`, `filter.separable_correlation_accelerated_x86_64`. They remain unregistered and unimplemented. Accelerated variants must satisfy NUM's final FP32-scaled four-ULP requirement and fallback rules without accumulating a separate budget per tap, axis, stage, or iteration. Threshold, ordering, boundary, copy, and output-support choices that must be exact cannot change approximately. Float64 accelerated paths retain Float64 input/output and exponent range; they do not first convert to Float32. Performance and CPU/ISA differential acceptance are not established here.

## Conceptual DAG (not an existing API)

`static parameters + descriptors -> preflight/demand -> declared data and control -> exact expression and declared rounding stages -> requested owned output/result`

This sketch introduces no implicit color conversion, alpha premultiplication, frequency correction, or zero-fill for missing data. Whole and staged-buffer requirements are those declared by the member and family.

## Sources and open items

[S01](../research-sources.md#s01).
