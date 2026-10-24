---
spec_schema_version: 1
id: FIL-11A
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
parent_id: FIL-11
function: log_kernel
proposed_operation_keys:
- filter.log_kernel_strict
- filter.log_kernel_accelerated_apple_silicon
- filter.log_kernel_accelerated_x86_64
numeric_reference: B
oracle_scope: mathematical_reference
---

# FIL-11A: log_kernel

Sampled LoG kernel with zero-DC correction. Status **Proposed / D1_draft**.

Inherits the [FIL-11 family contract](FIL-11_contract.md), [FILTER common contract](FILTER_common_contract.md), [NUM numerical/accuracy contract](../../01-numeric/op_specs/NUM_common_contract.md), and [FMT metadata/straight semantics](../../02-format-color/op_specs/FMT_common_contract.md). The family and member specifications together form the complete draft; do not override their RN, underflow, or accelerated-error requirements.

## Ports, shape, and semantics

No dynamic input → Float64 `kernel` with shape `[2r+1,2r+1]` and Float64 `dc_correction` with shape `[1]`.

Ports carry Results with fixed Tensor member schemas. Unless the member defines an explicit color interpretation, samples are raw numerical values; channel count does not imply RGB or alpha. Color interpretation is explicit configuration. Positive extents, axes and explicit broadcasting are checked during static construction. Materialized output tiles have independent allocations. FilterBands and FrequencyGrid are proposed SDK/workflow representations under the collection contract; lowering records each selected output's fixed reads and writes.

## Static parameters and valid domains

Explicit positive finite Float64 `sigma`; explicit Int64 `radius≥1`; explicit Bool `scale_normalized`; fixed spacing of one pixel. No constructor defaults.

Exceptional values and floating-point overflow follow the corresponding NUM operations and the [FILTER common contract](FILTER_common_contract.md). Do not reject NaN/Inf under a category-wide rule or convert ordinary floating-point overflow into failure. Static structural, parameter-type and capacity-arithmetic errors retain pre-execution diagnostics. Numerical-domain premises are unchecked; resource, cancellation and upstream failures retain their contracts. Every static parameter must be explicitly supplied with its type and valid domain; constructors have no implicit defaults. Floating-point parameters use their stored values.

## Mathematical reference and rounding boundaries

Numerical classification: **B**. See [FILTER_numeric_reference](FILTER_numeric_reference.md) for E/B/S.

For each tap, `v=RN64(((x²+y²-2sigma²)/sigma^4)*exp(-(x²+y²)/(2sigma²))/(2πsigma²) * (scale_normalized?sigma²:1))`; evaluate the complete real expression and round once to RN64. Store v in `kernel`. Expose `dc_correction=RN64(Σv)` as a separate diagnostic port. For strict zero-DC filtering, do not use this rounded correction to define B; use the exact sum of stored kernel values in the center-difference formula.

If the formula invokes another member, preserve the RN stages at the call site as specified here or by the family contract. Do not introduce undeclared rounding in internal temporary calculations. Signed zero, numerical-domain premises and explicit valid-input no-solution outcomes, gradual underflow, and output range follow the common contract and NUM.

## Fixed reads, algorithmic control and schemas

No Data inputs. Sigma and radius are static parameters. Symmetric taps share identical bits.

Boundary coordinates use the complete logical image; ROI and tile edges are not new image boundaries. Lowering closes mathematical sample support on fixed input member-tile grids. Data-dependent selection uses a finite static superset of every address the task may consume; runtime selection does not expand the Plan. Data and control are algorithmic inputs, with no separate Validation read set. Empty retained selections introduce no payload tasks. Output schemas derive only from static parameters and input schemas. Numerical, encoding and association premises remain unchecked under the common contract.

## Algorithm, resources, and execution

O(A) work with directed enclosures for exp and π. Do not normalize the sum even for small kernels.

In complexity notation, P=HW, C is the number of independent planes, A is the tap count, and T is the iteration count. Arbitrary-precision limb costs are additional; these are not measured benchmarks. The shared contract governs work/memory/stage admission, cancellation, failure status, and owner lifetime. A failed call publishes no successful Result export; previously retired call exports remain valid, and final RET results require successful Run completion. Do not spill automatically, reduce precision silently, or fall back to retired implementations.

## Oracle, fixtures, and acceptance

The kernel is mirror-symmetric with a negative center. `dc_correction` may be nonzero; the rounded tap values are not guaranteed to sum to exact zero.

The reference function evaluates the numerical formula on small inputs; it is neither a production kernel nor a complete port-schema/preflight simulator. ExactRational uses rational expressions with direct IEEE rounding. For transcendental functions, only DirectedMPFR results whose intervals close to one rounded value are strict goldens. Report Inconclusive when the interval does not determine rounding; do not substitute an approximation.

`fil11.transcend.log_kernel(sigma, radius, *, scale_normalized)` → [source](../../../../oracle/ops/filter/fil11/transcend.py).

Fixture mapping and evidence scope: see [oracle coverage](../oracle-coverage.md).

See the [oracle README](../../../../oracle/ops/filter/README.md) for reproduction and evidence levels. The listed self-checks cover only the reference program. Parameter boundaries, full rank/batch behavior, metadata, compiled ROI/tile selection, budgets, cancellation, and owners still require [runtime acceptance](FILTER_oracle_protocol.md). Passing an identity or constant fixture does not accept the full algorithm or all configurations.

## Backend and registration gates

The keys above are registered CPU operations through fixed invocation ABI 8 in `plugins/ops/05-filter/`. Strict execution uses certified enclosures with exact-expression fallback. Apple Silicon and x86_64 variants retain their named capability gates. Specification acceptance remains Proposed/D1. Accelerated variants must satisfy NUM final FP32-scaled four-ULP bounds and fallback rules; error budgets cannot be accumulated across taps, axes, stages, or iterations. Thresholds, ordering, boundaries, copies, and output support that require exact selection cannot change approximately.

Float64 accelerated execution retains Float64 inputs/outputs and exponent range; it does not first convert to Float32. Values outside the Float32-scale range use strict NUM fallback. Implementation status records runtime availability; validation and performance evidence are maintained separately from this normative specification.

## Conceptual DAG (not an existing API)

`static parameters + schemas → compile fixed reads/tasks → prepare → unchecked arithmetic with declared RN stages → complete tiles → ordered call retirement`

This diagram introduces no implicit color conversion, hidden alpha premultiplication, automatic spectral correction, or zero-filling of missing data. complete-input demand and multi-stage buffers are governed by this member and its family contract.
