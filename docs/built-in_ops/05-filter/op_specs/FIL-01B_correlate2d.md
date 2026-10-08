---
spec_schema_version: 1
id: FIL-01B
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
parent_id: FIL-01
function: correlate2d
proposed_operation_keys:
- filter.correlate2d_strict
- filter.correlate2d_accelerated_apple_silicon
- filter.correlate2d_accelerated_x86_64
numeric_reference: E
oracle_scope: mathematical_reference
research_sources:
- S01
- S02
---
# FIL-01B: correlate2d

Two-dimensional correlation over a floating-point field.

Inherit [FIL-01](FIL-01_contract.md), [FILTER common contract](FILTER_common_contract.md), [NUM numeric and precision contract](../../01-numeric/op_specs/NUM_common_contract.md), and [FMT metadata and straight-color contract](../../02-format-color/op_specs/FMT_common_contract.md). Together, the family and member specifications define the target. Do not override RN stages, underflow behavior, or accelerated error bounds.

## Ports, shape, and semantics

`input` is a floating-point numerical field and `kernel` is a Float64 `[Kh,Kw]` array; output is `output`. Non-spatial axes and planes are preserved. Spatial shape and global output origin follow the boundary contract.

## Explicit parameters and legal domain

`y_axis`, `x_axis`, `anchor_y`, `anchor_x`, `shape`, `boundary`, `cval`, `normalization`, and `bias` are all required. `shape` is `same`, `full`, or `valid`; `boundary` is explicit. `full` and `valid` require constant extension with `cval=0`. Normalization is `none`, `sum`, or `l1`. `bias` is a finite Float64 stored value. The kernel is Float64 `[Kh,Kw]`; anchor is required, including for odd kernels. No constructor defaults.

NUM special-value, floating-point overflow, signed-zero, payload, rounding, and precision behavior follows the corresponding operation and [FILTER common contract](FILTER_common_contract.md). Do not reject NaN/Inf under a family-wide rule or convert ordinary floating-point overflow into failure. Parameter-domain, structural, integer-overflow, resource, cancellation, and upstream errors retain their own contracts. Every parameter is explicit in the node; constructors have no implicit defaults. Interpret floating-point parameters as their stored values.

## Mathematical reference and rounding

Use the correlation coordinate convention from [boundary contract](FILTER_boundary_contract.md). Set N=1 for `none`, N=ΣK for `sum`, and N=Σ|K| for `l1`; reject a zero divisor for a requested normalization. In strict mode each sample is `RN_t(Σ K_i I_i/N + bias)`. Do not round products, sums, or division to an intermediate dtype. Correlation samples with the kernel in stored orientation and does not reverse it.

The numeric class and E/B/S meanings are defined in [FILTER numeric reference](FILTER_numeric_reference.md). Retain every declared rounding stage; helper calls do not introduce extra intermediate rounding unless the member says so. Signed zero, discrete choices, gradual underflow, and output range follow the member and NUM contracts.

Demand is stated for each requested output. Exact demand is the actual support after mapping coordinates through the boundary rule; do not substitute a larger rectangle or Whole read for convenience. Kernel, footprint, masks, and static selection parameters are control/validation inputs as specified below. Boundaries are relative to the complete logical image, never to an ROI or tile edge. Empty demand does not read payload, and any selection condition retains the required control dirty witness. Output descriptors derive from static parameters and input descriptors, not sample values.

Kernel and static parameters are validation/control inputs. Constant-boundary taps do not read source data. Direct execution is O(PCA) with exact sparse accumulation. FFT or separable execution may replace it only with proof of the same final E rounding.

Complexity uses P=HW samples, C independent planes, A taps, and T iterations where applicable; arbitrary-precision limb cost is additional and is not a measured benchmark. Follow FILTER admission, work/memory/stage accounting, cancellation, failure, and owner-lifetime rules. Failure publishes no partial Value or CompleteBundle. Do not spill implicitly, lower precision silently, or fall back to retired implementations.

## Oracle, fixtures, and acceptance

The reference case uses input row `[1,2,4]`, kernel row `[1,2]`, anchor `(0,0)`, constant-zero boundary, and same shape. Correlation outputs `[5,10,4]`. Also cover off-center even kernels, full/valid global origins, rejected zero sum normalization, legal negative sum, and zero taps that must not read poisoned source data. Reference callable: `convolve2d(image,kernel,anchor=(0,0),*,direction='convolve',normalization='none',bias=0,boundary='reflect_half',cval=0,output_shape='same',dtype='float64')` in [spatial.py](../../../../oracle/ops/filter/oracles/spatial.py). For correlation, pass `direction='correlate'`. Oracle helper defaults do not define graph-node constructors; every node parameter remains explicitly required.

The linked reference function is a small mathematical oracle, not a production kernel or a complete port-schema/preflight simulator. ExactRational uses exact rational expressions followed by IEEE rounding. For transcendental functions, only a DirectedMPFR interval that uniquely determines the result is a strict golden; otherwise report Inconclusive. Fixture identifiers describe reference-case scope only and make no claim about runtime implementation, full rank/batch behavior, metadata, ROI/dirty behavior, budgets, cancellation, or ownership. See the [runtime acceptance protocol](FILTER_oracle_protocol.md).

See the member's linked reference source and oracle README.

Fixture coverage identifiers: `boundary_reflect_half`, `boundary_reflect_whole`, `boundary_wrap`, `boundary_clamp`, `asymmetric_correlate_float32`, `asymmetric_correlate_float64`, `convolution_origins`.

## Backend and registration

The proposed keys are `filter.correlate2d_strict`, `filter.correlate2d_accelerated_apple_silicon`, `filter.correlate2d_accelerated_x86_64`. They remain unregistered and unimplemented. Accelerated variants must satisfy NUM's final FP32-scaled four-ULP requirement and fallback rules without accumulating a separate budget per tap, axis, stage, or iteration. Threshold, ordering, boundary, copy, and output-support choices that must be exact cannot change approximately. Float64 accelerated paths retain Float64 input/output and exponent range; they do not first convert to Float32. Performance and CPU/ISA differential acceptance are not established here.

## Conceptual DAG (not an existing API)

`static parameters + descriptors -> preflight/demand -> declared data and control -> exact expression and declared rounding stages -> requested owned output/result`

This sketch introduces no implicit color conversion, alpha premultiplication, frequency correction, or zero-fill for missing data. Whole and staged-buffer requirements are those declared by the member and family.

## Sources and open items

[S01](../research-sources.md#s01), [S02](../research-sources.md#s02).
