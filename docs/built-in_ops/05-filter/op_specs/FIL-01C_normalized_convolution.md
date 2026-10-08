---
spec_schema_version: 1
id: FIL-01C
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
parent_id: FIL-01
function: normalized_convolution
proposed_operation_keys:
- filter.normalized_convolution_strict
- filter.normalized_convolution_accelerated_apple_silicon
- filter.normalized_convolution_accelerated_x86_64
numeric_reference: E
oracle_scope: mathematical_reference
research_sources:
- S01
- S02
---
# FIL-01C: normalized_convolution

Positive-kernel convolution with participation weights.

Inherit [FIL-01](FIL-01_contract.md), [FILTER common contract](FILTER_common_contract.md), [NUM numeric and precision contract](../../01-numeric/op_specs/NUM_common_contract.md), and [FMT metadata and straight-color contract](../../02-format-color/op_specs/FMT_common_contract.md). Together, the family and member specifications define the target. Do not override RN stages, underflow behavior, or accelerated error bounds.

## Ports, shape, and semantics

Inputs are `input`, `mask`, and `kernel`; outputs are `output` and UInt8 `valid` with value 0 or 1. Mask and input must have matching spatial/batch shapes or an explicitly declared singleton component broadcast. Output matches the statistic group shape.

## Explicit parameters and legal domain

Require axes and anchor, `shape=same`, an explicit boundary (`truncate` is supported), and `empty` chosen from `copy_center`, `zero`, or `error`. Do not expose bias or normalization parameters. Inputs are a raw field, participation mask, and nonnegative kernel. No constructor defaults.

NUM special-value, floating-point overflow, signed-zero, payload, rounding, and precision behavior follows the corresponding operation and [FILTER common contract](FILTER_common_contract.md). Do not reject NaN/Inf under a family-wide rule or convert ordinary floating-point overflow into failure. Parameter-domain, structural, integer-overflow, resource, cancellation, and upstream errors retain their own contracts. Every parameter is explicit in the node; constructors have no implicit defaults. Interpret floating-point parameters as their stored values.

## Mathematical reference and rounding

For each output, let `D=Σ K M`. Require `K≥0` and `M∈[0,1]`. If `D>0`, return `RN_t(Σ K M I / D)` and `valid=1`. If `D=0`, apply the required `empty` strategy and return `valid=0`. When `M=0`, do not read or validate I; when `K=0`, do not read M or I. Validate the complete kernel. Constant-boundary exterior taps use `M=1`; truncated exterior taps do not participate.

The numeric class and E/B/S meanings are defined in [FILTER numeric reference](FILTER_numeric_reference.md). Retain every declared rounding stage; helper calls do not introduce extra intermediate rounding unless the member says so. Signed zero, discrete choices, gradual underflow, and output range follow the member and NUM contracts.

Demand is stated for each requested output. Exact demand is the actual support after mapping coordinates through the boundary rule; do not substitute a larger rectangle or Whole read for convenience. Kernel, footprint, masks, and static selection parameters are control/validation inputs as specified below. Boundaries are relative to the complete logical image, never to an ROI or tile edge. Empty demand does not read payload, and any selection condition retains the required control dirty witness. Output descriptors derive from static parameters and input descriptors, not sample values.

Per output, read the kernel and actual mask control first, then read data only where `K*M>0`. Preserve all participating mask samples as dirty witnesses. Complexity is O(PCA) with two exact sums. Do not reuse this profile for signed derivative kernels.

Complexity uses P=HW samples, C independent planes, A taps, and T iterations where applicable; arbitrary-precision limb cost is additional and is not a measured benchmark. Follow FILTER admission, work/memory/stage accounting, cancellation, failure, and owner-lifetime rules. Failure publishes no partial Value or CompleteBundle. Do not spill implicitly, lower precision silently, or fall back to retired implementations.

## Oracle, fixtures, and acceptance

Use input `[1,NaN,5]`, mask `[1,0,1]`, and a three-tap unit kernel: the center output is 3 and succeeds. With an all-zero mask, exercise each empty policy. A NaN mask and a negative kernel entry are invalid.

The linked reference function is a small mathematical oracle, not a production kernel or a complete port-schema/preflight simulator. ExactRational uses exact rational expressions followed by IEEE rounding. For transcendental functions, only a DirectedMPFR interval that uniquely determines the result is a strict golden; otherwise report Inconclusive. Fixture identifiers describe reference-case scope only and make no claim about runtime implementation, full rank/batch behavior, metadata, ROI/dirty behavior, budgets, cancellation, or ownership. See the [runtime acceptance protocol](FILTER_oracle_protocol.md).

See the member's linked reference source and oracle README.

Fixture coverage identifiers: `normalized_mask_poison_float32`, `normalized_mask_poison_float64`.

Reference callable: `normalized_convolution(image,mask,kernel,anchor=(0,0),*,boundary='truncate',cval=0,empty='copy_center',dtype='float64')` in [spatial.py](../../../../oracle/ops/filter/oracles/spatial.py). These oracle helper defaults do not define graph-node constructors; every node parameter remains explicitly required.

## Backend and registration

The proposed keys are `filter.normalized_convolution_strict`, `filter.normalized_convolution_accelerated_apple_silicon`, `filter.normalized_convolution_accelerated_x86_64`. They remain unregistered and unimplemented. Accelerated variants must satisfy NUM's final FP32-scaled four-ULP requirement and fallback rules without accumulating a separate budget per tap, axis, stage, or iteration. Threshold, ordering, boundary, copy, and output-support choices that must be exact cannot change approximately. Float64 accelerated paths retain Float64 input/output and exponent range; they do not first convert to Float32. Performance and CPU/ISA differential acceptance are not established here.

## Conceptual DAG (not an existing API)

`static parameters + descriptors -> preflight/demand -> declared data and control -> exact expression and declared rounding stages -> requested owned output/result`

This sketch introduces no implicit color conversion, alpha premultiplication, frequency correction, or zero-fill for missing data. Whole and staged-buffer requirements are those declared by the member and family.

## Sources and open items

[S01](../research-sources.md#s01), [S02](../research-sources.md#s02).
