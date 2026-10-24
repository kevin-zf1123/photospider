---
spec_schema_version: 1
id: FIL-03B
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
parent_id: FIL-03
function: box_mean
proposed_operation_keys:
- filter.box_mean_strict
- filter.box_mean_accelerated_apple_silicon
- filter.box_mean_accelerated_x86_64
numeric_reference: E
oracle_scope: mathematical_reference
research_sources:
- S01
---
# FIL-03B: box_mean

Rectangular mean.

Inherit [FIL-03](FIL-03_contract.md), [FILTER common contract](FILTER_common_contract.md), [NUM numeric and precision contract](../../01-numeric/op_specs/NUM_common_contract.md), and [FMT metadata and straight-color contract](../../02-format-color/op_specs/FMT_common_contract.md). Together, the family and member specifications define the target. Do not override RN stages, underflow behavior, or accelerated error bounds.

## Ports, shape, and semantics

Input `input` is a raw floating-point field; output `output` has the same shape.

## Explicit parameters and legal domain

Require axes, positive integer `height` and `width`, an explicit anchor, boundary, and `cval`. Shape is fixed to `same`; mean supports the declared `truncate` boundary. No constructor defaults.

NUM special-value, floating-point overflow, signed-zero, payload, rounding, and precision behavior follows the corresponding operation and [FILTER common contract](FILTER_common_contract.md). Do not reject NaN/Inf under a family-wide rule or convert ordinary floating-point overflow into failure. Static structural, parameter-type and capacity-arithmetic errors retain pre-execution diagnostics. Numerical-domain premises are unchecked; resource, cancellation and upstream failures retain their contracts. Every parameter is explicit in the node; constructors have no implicit defaults. Interpret floating-point parameters as their stored values.

## Mathematical reference and rounding

Every footprint tap has unit weight. A tap `(j,i)` samples `(y+j-anchor_y,x+i-anchor_x)` using the correlation coordinate convention, including even and off-center footprints. Return `RN_t(ΣI/N_eff)`, where `N_eff` is the number of in-domain taps under truncation and otherwise `height*width`. The anchor lies inside the footprint, so `N_eff≥1`.

The numeric class and E/B/S meanings are defined in [FILTER numeric reference](FILTER_numeric_reference.md). Retain every declared rounding stage; helper calls do not introduce extra intermediate rounding unless the member says so. Signed zero, discrete choices, gradual underflow, and output range follow the member and NUM contracts.

Boundary coordinates use the complete logical image; ROI and tile edges are not new image boundaries. Lowering closes mathematical sample support on fixed input member-tile grids. Data-dependent selection uses a finite static superset of every address the task may consume; runtime selection does not expand the Plan. Data and control are algorithmic inputs, with no separate Validation read set. Empty retained selections introduce no payload tasks. Output schemas derive only from static parameters and input schemas. Numerical, encoding and association premises remain unchecked under the common contract.

The mapped support is the complete rectangular footprint; repeated extension preserves mathematical multiplicity, while truncation reads no out-of-domain samples. A direct O(PCA) loop is the oracle. Sliding or prefix methods with O(PC) work require an exact accumulator and must declare any expanded Conservative or complete-input scan support.

Complexity uses P=HW samples, C independent planes, A taps, and T iterations where applicable; arbitrary-precision limb cost is additional and is not a measured benchmark. Follow FILTER admission, work/memory/stage accounting, cancellation, failure, and owner-lifetime rules. A failed call publishes no successful Result export; previously retired call exports remain valid, and final RET results require successful Run completion. Do not spill implicitly, lower precision silently, or fall back to retired implementations.

## Oracle, fixtures, and acceptance

For a constant field of 2 and a 3x3 footprint, mean is 2 (the corresponding FIL-03A sum is 18). Also cover a single-pixel image, a kernel larger than the image, truncated corner sample counts, and even-sized off-center footprints.

The linked reference function is a small mathematical oracle, not a production kernel or a complete port-schema/preflight simulator. ExactRational uses exact rational expressions followed by IEEE rounding. For transcendental functions, only a DirectedMPFR interval that uniquely determines the result is a strict golden; otherwise report Inconclusive. Fixture identifiers describe reference-case scope only and make no claim about runtime implementation, full rank/batch behavior, metadata, compiled ROI/tile behavior, budgets, cancellation, or ownership. See the [runtime acceptance protocol](FILTER_oracle_protocol.md).

See the member's linked reference source and oracle README.

Fixture coverage identifiers: `box_True_float32`, `box_True_float64`.

Reference callable: `box(image,height,width,anchor,*,mean=True,boundary='reflect_half',cval=0,dtype='float64')` in [spatial.py](../../../../oracle/ops/filter/fil03/spatial.py). These oracle helper defaults do not define graph-node constructors; every node parameter remains explicitly required.

## Backend and registration

The proposed keys are `filter.box_mean_strict`, `filter.box_mean_accelerated_apple_silicon`, `filter.box_mean_accelerated_x86_64`. These keys are registered CPU operations through fixed invocation ABI 8 in `plugins/ops/05-filter/`. Strict evaluation uses certified enclosures with exact-expression fallback; accelerated keys require their named CPU capability. The specification remains Proposed/D1. Accelerated variants must satisfy NUM's final FP32-scaled four-ULP requirement and fallback rules without accumulating a separate budget per tap, axis, stage, or iteration. Threshold, ordering, boundary, copy, and output-support choices that must be exact cannot change approximately. Float64 accelerated paths retain Float64 input/output and exponent range; they do not first convert to Float32. Implementation status records runtime availability; performance measurements and validation evidence are maintained separately from this normative specification.

## Conceptual DAG (not an existing API)

`static parameters + schemas -> compile fixed reads/tasks -> prepare -> unchecked arithmetic with declared rounding stages -> complete tiles -> ordered call retirement`

This sketch introduces no implicit color conversion, alpha premultiplication, frequency correction, or zero-fill for missing data. complete-input and staged-buffer requirements are those declared by the member and family.

## Sources and open items

[S01](../research-sources.md#s01).
