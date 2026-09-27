---
spec_schema_version: 1
id: FIL-04B
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
parent_id: FIL-04
function: gaussian_filter
proposed_operation_keys:
- filter.gaussian_filter_strict
- filter.gaussian_filter_accelerated_apple_silicon
- filter.gaussian_filter_accelerated_x86_64
numeric_reference: B+E
oracle_scope: mathematical_reference
research_sources:
- S03
- S25
---
# FIL-04B: gaussian_filter

Apply the baked64 Gaussian profile.

Inherit [FIL-04](FIL-04_contract.md), [FILTER common contract](FILTER_common_contract.md), [NUM numeric and precision contract](../../01-numeric/op_specs/NUM_common_contract.md), and [FMT metadata and straight-color contract](../../02-format-color/op_specs/FMT_common_contract.md). Together, the family and member specifications define the target. Do not override RN stages, underflow behavior, or accelerated error bounds.

## Ports, shape, and semantics

Input `input` is a raw field; output `output` has the same shape.

## Explicit parameters and legal domain

Require `sigma_x/y`, `radius_x/y`, axes, boundary, and `cval`. Sigma controls weight decay; radius independently controls finite support. Zero sigma requires zero radius; positive sigma permits radius zero. The operation has no arbitrary bias and no constructor defaults.

NUM special-value, floating-point overflow, signed-zero, payload, rounding, and precision behavior follows the corresponding operation and [FILTER common contract](FILTER_common_contract.md). Do not reject NaN/Inf under a family-wide rule or convert ordinary floating-point overflow into failure. Parameter-domain, structural, integer-overflow, resource, cancellation, and upstream errors retain their own contracts. Every parameter is explicit in the node; constructors have no implicit defaults. Interpret floating-point parameters as their stored values.

## Mathematical reference and rounding

Treat `kx` and `ky` as exact binary64 constants. Return `RN_t(Σ ky*kx*I / ((Σky)(Σkx)))` with one final rounding. Boundary handling follows FIL-02A. This is not the result of filtering an already rounded intermediate Gaussian image. If both radii are zero, perform an exact bit copy and do not read or validate other samples.

The numeric class and E/B/S meanings are defined in [FILTER numeric reference](FILTER_numeric_reference.md). Retain every declared rounding stage; helper calls do not introduce extra intermediate rounding unless the member says so. Signed zero, discrete choices, gradual underflow, and output range follow the member and NUM contracts.

Demand is stated for each requested output. Exact demand is the actual support after mapping coordinates through the boundary rule; do not substitute a larger rectangle or Whole read for convenience. Kernel, footprint, masks, and static selection parameters are control/validation inputs as specified below. Boundaries are relative to the complete logical image, never to an ROI or tile edge. Empty demand does not read payload, and any selection condition retains the required control dirty witness. Output descriptors derive from static parameters and input descriptors, not sample values.

Use the Cartesian product of nonzero 1D taps; the kernel is derived static control data. Zero sigma only removes support on its own axis. There is no hidden UI maximum for large sigma, though indexing, work, and resource budgets still apply. Execution follows FIL-02A.

Complexity uses P=HW samples, C independent planes, A taps, and T iterations where applicable; arbitrary-precision limb cost is additional and is not a measured benchmark. Follow FILTER admission, work/memory/stage accounting, cancellation, failure, and owner-lifetime rules. Failure publishes no partial Value or CompleteBundle. Do not spill implicitly, lower precision silently, or fall back to retired implementations.

## Oracle, fixtures, and acceptance

Verify exact preservation of constants and impulse reflection. Compare Float32 and Float64 results with a baked64 rational oracle. Do not assert that two truncated blurs equal one blur with sigma `sqrt(s1²+s2²)`.

The linked reference function is a small mathematical oracle, not a production kernel or a complete port-schema/preflight simulator. ExactRational uses exact rational expressions followed by IEEE rounding. For transcendental functions, only a DirectedMPFR interval that uniquely determines the result is a strict golden; otherwise report Inconclusive. Fixture identifiers describe reference-case scope only and make no claim about runtime implementation, full rank/batch behavior, metadata, ROI/dirty behavior, budgets, cancellation, or ownership. See the [runtime acceptance protocol](FILTER_oracle_protocol.md).

The reference helpers are `gaussian_kernel(sigma,radius)` in [transcend.py](../../../../oracle/ops/filter/oracles/transcend.py) and `separable(image,kx,ky,anchor=(0,0),**kwargs)` in [spatial.py](../../../../oracle/ops/filter/oracles/spatial.py). See also the [oracle README](../../../../oracle/ops/filter/README.md).

Fixture coverage identifiers: `gaussian_constant`, `straight_alpha_zero_hidden_color`, `straight_all_zero_alpha`, `straight_alpha_underflow_hidden_color`. This catalog records fixture scope only; it makes no current pass claim.

## Backend and registration

The proposed keys are `filter.gaussian_filter_strict`, `filter.gaussian_filter_accelerated_apple_silicon`, `filter.gaussian_filter_accelerated_x86_64`. They remain unregistered and unimplemented. Accelerated variants must satisfy NUM's final FP32-scaled four-ULP requirement and fallback rules without accumulating a separate budget per tap, axis, stage, or iteration. Threshold, ordering, boundary, copy, and output-support choices that must be exact cannot change approximately. Float64 accelerated paths retain Float64 input/output and exponent range; they do not first convert to Float32. Performance and CPU/ISA differential acceptance are not established here.

## Conceptual DAG (not an existing API)

`static parameters + descriptors -> preflight/demand -> declared data and control -> exact expression and declared rounding stages -> requested owned output/result`

This sketch introduces no implicit color conversion, alpha premultiplication, frequency correction, or zero-fill for missing data. Whole and staged-buffer requirements are those declared by the member and family.

## Sources and open items

[S03](../research-sources.md#s03), [S25](../research-sources.md#s25).
