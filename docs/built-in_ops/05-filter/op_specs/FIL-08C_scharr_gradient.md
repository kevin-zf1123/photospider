---
spec_schema_version: 1
id: FIL-08C
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
parent_id: FIL-08
function: scharr_gradient
proposed_operation_keys:
- filter.scharr_gradient_strict
- filter.scharr_gradient_accelerated_apple_silicon
- filter.scharr_gradient_accelerated_x86_64
numeric_reference: E
oracle_scope: mathematical_reference
---

# FIL-08C: scharr_gradient

Scharr gradient. Status **Proposed / D1_draft**.

Inherits [FIL-08 family contract](FIL-08_contract.md) and [FILTER common contract](FILTER_common_contract.md) and [NUM numerical/accuracy contract](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT metadata/straight semantics](../../02-format-color/op_specs/FMT_common_contract.md). The family and member specifications together form the complete draft; do not override their RN, underflow, or accelerated-error requirements.

## Ports, shape, and semantics

`input` → independently requestable `gx,gy`, each with the input shape. No complete-color assertion is made.

Ports carry Results with fixed Tensor member schemas. Unless the member defines an explicit color interpretation, samples are raw numerical values; channel count does not imply RGB or alpha. Color interpretation is explicit configuration. Positive extents, axes and explicit broadcasting are checked during static construction. Materialized output tiles have independent allocations. FilterBands and FrequencyGrid are proposed SDK/workflow representations under the collection contract; lowering records each selected output's fixed reads and writes.

## Static parameters and valid domains

Explicit Int64 `y_axis` and `x_axis` values; positive finite Float64 `spacing_x` and `spacing_y`; explicitly selected `normalization` (`unit_ramp` or `raw`). Boundary is fixed by the named profile to `reflect_half`, with `cval=0`. The constructor supplies no defaults.

Exceptional values and floating-point overflow follow the corresponding NUM operations and the [FILTER common contract](FILTER_common_contract.md). Do not reject NaN/Inf under a category-wide rule or convert ordinary floating-point overflow into failure. Static structural, parameter-type and capacity-arithmetic errors retain pre-execution diagnostics. Numerical-domain premises are unchecked; resource, cancellation and upstream failures retain their contracts. Every static parameter must be explicitly supplied with its type and valid domain; constructors have no implicit defaults. Floating-point parameters use their stored values.

## Mathematical reference and rounding boundaries

Numerical classification: **E**. See [FILTER_numeric_reference](FILTER_numeric_reference.md) for E/B/S.

Use correlation with x-derivative kernel [-1,0,1] and y-smoothing kernel [3,10,3]; `gy` is the transpose. For `unit_ramp`, `gx=RN_t(ΣKx I/(32*spacing_x))` with the analogous y expression. `raw` omits division by 32 but still divides by spacing. Each component is evaluated exactly and rounded once; the zero tap is not read.

If the formula invokes another member, preserve the RN stages at the call site as specified here or by the family contract. Do not introduce undeclared rounding in internal temporary calculations. Signed zero, numerical-domain premises and explicit valid-input no-solution outcomes, gradual underflow, and output range follow the common contract and NUM.

## Fixed reads, algorithmic control and schemas

`gx` and `gy` have independent sparse support. Spacing affects descriptors/static planning only. A request for `gx` alone does not prepare the `gy` support.

Boundary coordinates use the complete logical image; ROI and tile edges are not new image boundaries. Lowering closes mathematical sample support on fixed input member-tile grids. Data-dependent selection uses a finite static superset of every address the task may consume; runtime selection does not expand the Plan. Data and control are algorithmic inputs, with no separate Validation read set. Empty retained selections introduce no payload tasks. Output schemas derive only from static parameters and input schemas. Numerical, encoding and association premises remain unchecked under the common contract.

## Algorithm, resources, and execution

O(PC) work with a constant stencil; resources are limited to local read windows and exact accumulation.

In complexity notation, P=HW, C is the number of independent planes, A is the tap count, and T is the iteration count. Arbitrary-precision limb costs are additional; these are not measured benchmarks. The shared contract governs work/memory/stage admission, cancellation, failure status, and owner lifetime. A failed call publishes no successful Result export; previously retired call exports remain valid, and final RET results require successful Run completion. Do not spill automatically, reduce precision silently, or fall back to retired implementations.

## Oracle, fixtures, and acceptance

For an interior ramp I=x, `unit_ramp` gives gx=1/dx and gy=0; verify the positive y direction. A one-pixel clamped input yields zero. Include separate poisoned-input tests for the two outputs and boundary fixtures.

The reference function evaluates the numerical formula on small inputs; it is neither a production kernel nor a complete port-schema/preflight simulator. ExactRational uses rational expressions with direct IEEE rounding. For transcendental functions, only DirectedMPFR results whose intervals close to one rounded value are strict goldens. Report Inconclusive when the interval does not determine rounding; do not substitute an approximation.

`fil08.spatial.gradient(image, method, *, dx, dy, normalization, boundary, cval, dtype)` → [source](../../../../oracle/ops/filter/fil08/spatial.py) .

Fixture mapping and evidence scope: see [oracle coverage](../oracle-coverage.md).

See the [oracle README](../../../../oracle/ops/filter/README.md) for reproduction and evidence levels. The listed self-checks cover only the reference program. Parameter boundaries, full rank/batch behavior, metadata, compiled ROI/tile selection, budgets, cancellation, and owners still require [runtime acceptance](FILTER_oracle_protocol.md). Passing an identity or constant fixture does not accept the full algorithm or all configurations.

## Backend and registration gates

The keys above are naming proposals only; strict and both CPU accelerated variants are not registered or implemented. Accelerated variants must satisfy NUM final FP32-scaled four-ULP bounds and fallback rules; error budgets cannot be accumulated across taps, axes, stages, or iterations. Thresholds, ordering, boundaries, copies, and output support that require exact selection cannot change approximately.

Float64 accelerated execution retains Float64 inputs/outputs and exponent range; it does not first convert to Float32. Values outside the Float32-scale range use strict NUM fallback. No kernel, CPU, or ISA performance or differential acceptance was performed here.

## Conceptual DAG (not an existing API)

`static parameters + schemas → compile fixed reads/tasks → prepare → unchecked arithmetic with declared RN stages → complete tiles → ordered call retirement`

This diagram introduces no implicit color conversion, hidden alpha premultiplication, automatic spectral correction, or zero-filling of missing data. complete-input demand and multi-stage buffers are governed by this member and its family contract.
