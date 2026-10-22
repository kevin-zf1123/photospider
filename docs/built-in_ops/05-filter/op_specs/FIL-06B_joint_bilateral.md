---
spec_schema_version: 1
id: FIL-06B
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D2_draft
implementation_status: not_implemented
parent_id: FIL-06
function: joint_bilateral
proposed_operation_keys:
- filter.joint_bilateral_strict
- filter.joint_bilateral_accelerated_apple_silicon
- filter.joint_bilateral_accelerated_x86_64
numeric_reference: B+E
oracle_scope: mathematical_reference
---

# FIL-06B: joint_bilateral

Joint bilateral filter. Status **Proposed / D2_draft**.

Inherits [FIL-06 family contract](FIL-06_contract.md) and [FILTER common contract](FILTER_common_contract.md) and [NUM numerical/accuracy contract](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT metadata/straight semantics](../../02-format-color/op_specs/FMT_common_contract.md). The family and member specifications together form the complete draft; do not override their RN, underflow, or accelerated-error requirements.

## Ports, shape, and semantics

`input,guide` → `output` with the same shape as `input`; `guide` matches input spatial and batch axes and has an explicit component dimension of 1..4.

Ports carry Results with fixed Tensor member schemas. Unless the member defines an explicit color interpretation, samples are raw numerical values; channel count does not imply RGB or alpha. Color interpretation is explicit configuration. Positive extents, axes and explicit broadcasting are checked during static construction. Materialized output tiles have independent allocations. FilterBands and FrequencyGrid are proposed SDK/workflow representations under the collection contract; lowering records each selected output's fixed reads and writes.

## Static parameters and valid domains

`radius_y` and `radius_x` are nonnegative Int64; `sigma_space_y`, `sigma_space_x`, and `sigma_range` are positive finite Float64. `metric_scale` is an explicitly supplied positive finite Float64 vector. `axis` explicitly identifies the two spatial axes. Callers must explicitly select one boundary from `reflect_half`, `reflect_whole`, `clamp`, `wrap`, or `truncate`.

Exceptional values and floating-point overflow follow the corresponding NUM operations and the [FILTER common contract](FILTER_common_contract.md). Do not reject NaN/Inf under a category-wide rule or convert ordinary floating-point overflow into failure. Static structural, parameter-type and capacity-arithmetic errors retain pre-execution diagnostics. Numerical-domain premises are unchecked; resource, cancellation and upstream failures retain their contracts. Every static parameter must be explicitly supplied with its type and valid domain; constructors have no implicit defaults. Floating-point parameters use their stored values.

## Mathematical reference and rounding boundaries

Numerical classification: **B+E**. See [FILTER_numeric_reference](FILTER_numeric_reference.md) for E/B/S definitions.

For tap d, `ws=RN64(exp(-dy²/(2sy²)-dx²/(2sx²)))`; compute `Dg=Σ((g(q+d,c)-g(q,c))*metric_scale[c])²` exactly; `wr=RN64(exp(-Dg/(2sr²)))`; compute w=ws*wr exactly; output `RN_t(ΣwI/Σw)`. The center weight is 1, so support is nonempty. FIL-06A processes each plane independently and self-guides from that plane; FIL-06B reuses the explicit guide for every output component and does not infer RGBA semantics.

If the formula invokes another member, preserve the RN stages at the call site as specified here or by the family contract. Do not introduce undeclared rounding in internal temporary calculations. Signed zero, numerical-domain premises and explicit valid-input no-solution outcomes, gradual underflow, and output range follow the common contract and NUM.

## Fixed reads, algorithmic control and schemas

The output footprint reads input and guide samples and the center guide. When ws=0, skip dynamic guide and input reads. Other guide samples remain Control even if wr underflows to zero; consume input only when w>0. Radius zero is an explicit identity copy.

Boundary coordinates use the complete logical image; ROI and tile edges are not new image boundaries. Lowering closes mathematical sample support on fixed input member-tile grids. Data-dependent selection uses a finite static superset of every address the task may consume; runtime selection does not expand the Plan. Data and control are algorithmic inputs, with no separate Validation read set. Empty retained selections introduce no payload tasks. Output schemas derive only from static parameters and input schemas. Numerical, encoding and association premises remain unchecked under the common contract.

## Algorithm, resources, and execution

O(PA*(C+G)); use baked exponential coefficients and exact weighted accumulation. Grid/lattice approximations, if added, require separate keys.

In complexity notation, P=HW, C is the number of independent planes, A is the tap count, and T is the iteration count. Arbitrary-precision limb costs are additional; these are not measured benchmarks. The shared contract governs work/memory/stage admission, cancellation, failure status, and owner lifetime. A failed call publishes no successful Result export; previously retired call exports remain valid, and final RET results require successful Run completion. Do not spill automatically, reduce precision silently, or fall back to retired implementations.

## Oracle, fixtures, and acceptance

A constant guide reduces to the same positive-weight spatial blur. Equal center and neighbor guides have zero range distance. Changing an unselected joint-guide component does not change output. Scaling the guide by s requires inverse scaling of range sigma or metric scale.

The reference function evaluates the numerical formula on small inputs; it is neither a production kernel nor a complete port-schema/preflight simulator. ExactRational uses rational expressions with direct IEEE rounding. For transcendental functions, only DirectedMPFR results whose intervals close to one rounded value are strict goldens. Report Inconclusive when the interval does not determine rounding; do not substitute an approximation.

`fil06.transcend.bilateral(image, guide, *, radius_y, radius_x, sigma_y, sigma_x, sigma_range, metric_scale, boundary, dtype)` → [source](../../../../oracle/ops/filter/fil06/transcend.py) .

Fixture mapping and evidence scope: see [oracle coverage](../oracle-coverage.md).

See the [oracle README](../../../../oracle/ops/filter/README.md) for reproduction and evidence levels. The listed self-checks cover only the reference program. Parameter boundaries, full rank/batch behavior, metadata, compiled ROI/tile selection, budgets, cancellation, and owners still require [runtime acceptance](FILTER_oracle_protocol.md). Passing an identity or constant fixture does not accept the full algorithm or all configurations.

## Backend and registration gates

The keys above are naming proposals only; strict and both CPU accelerated variants are not registered or implemented. Accelerated variants must satisfy NUM final FP32-scaled four-ULP bounds and fallback rules; error budgets cannot be accumulated across taps, axes, stages, or iterations. Thresholds, ordering, boundaries, copies, and output support that require exact selection cannot change approximately.

Float64 accelerated execution retains Float64 inputs/outputs and exponent range; it does not first convert to Float32. Values outside the Float32-scale range use strict NUM fallback. No kernel, CPU, or ISA performance or differential acceptance was performed here.

## Conceptual DAG (not an existing API)

`static parameters + schemas → compile fixed reads/tasks → prepare → unchecked arithmetic with declared RN stages → complete tiles → ordered call retirement`

This diagram introduces no implicit color conversion, hidden alpha premultiplication, automatic spectral correction, or zero-filling of missing data. complete-input demand and multi-stage buffers are governed by this member and its family contract.
