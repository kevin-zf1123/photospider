---
spec_schema_version: 1
id: FIL-07B
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D2_draft
implementation_status: not_implemented
parent_id: FIL-07
function: guided_vector
proposed_operation_keys:
- filter.guided_vector_strict
- filter.guided_vector_accelerated_apple_silicon
- filter.guided_vector_accelerated_x86_64
numeric_reference: 'S: coefficients RN64; final E'
oracle_scope: mathematical_reference
---

# FIL-07B: guided_vector

Vector guided filter. Status **Proposed / D2_draft**.

Inherits [FIL-07 family contract](FIL-07_contract.md) and [FILTER common contract](FILTER_common_contract.md) and [NUM numerical/accuracy contract](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT metadata/straight semantics](../../02-format-color/op_specs/FMT_common_contract.md). The family and member specifications together form the complete draft; do not override their RN, underflow, or accelerated-error requirements.

## Ports, shape, and semantics

`input,guide` → `output`; input has independent C planes. Guide has 1..4 components, matches H/W and batch dimensions, and uses a distinct component_axis.

Ports carry Results with fixed Tensor member schemas. Unless the member defines an explicit color interpretation, samples are raw numerical values; channel count does not imply RGB or alpha. Color interpretation is explicit configuration. Positive extents, axes and explicit broadcasting are checked during static construction. Materialized output tiles have independent allocations. FilterBands and FrequencyGrid are proposed SDK/workflow representations under the collection contract; lowering records each selected output's fixed reads and writes.

## Static parameters and valid domains

`radius_y` and `radius_x` are nonnegative Int64. `epsilon` is a positive finite Float64 in squared units of the metric-scaled guide. `guide_metric_scale` is an explicitly supplied positive finite Float64 scalar or vector. The boundary profile is fixed to clipped/truncate two-window support.

Exceptional values and floating-point overflow follow the corresponding NUM operations and the [FILTER common contract](FILTER_common_contract.md). Do not reject NaN/Inf under a category-wide rule or convert ordinary floating-point overflow into failure. Static structural, parameter-type and capacity-arithmetic errors retain pre-execution diagnostics. Numerical-domain premises are unchecked; resource, cancellation and upstream failures retain their contracts. Every static parameter must be explicitly supplied with its type and valid domain; constructors have no implicit defaults. Floating-point parameters use their stored values.

## Mathematical reference and rounding boundaries

Numerical classification: **S: coefficients RN64; final E**. See [FILTER_numeric_reference](FILTER_numeric_reference.md) for E/B/S.

Let B_k be the image-clipped box centered at k, and let g be the exactly scaled guide. Evaluate exactly `mu_g,mu_p,Cgg=mean(ggᵀ)-mu_g mu_gᵀ,Cgp=mean(gp)-mu_g mu_p` .`a_k=RN64_vec((Cgg+epsilon I)^-1 Cgp)` after an exact matrix solve, rounding each component once to RN64; `b_k=RN64(mu_p-dot(a_k,mu_g))` (using the already rounded a). Output `RN_t(dot(mean_{k∈Bq}(a_k),g_q)+mean(b_k))`; both means are exact. The vector SPD solution is unique and uses no arbitrary epsilon jitter; the scalar case is the same 1×1 formula.

If the formula invokes another member, preserve the RN stages at the call site as specified here or by the family contract. Do not introduce undeclared rounding in internal temporary calculations. Signed zero, numerical-domain premises and explicit valid-input no-solution outcomes, gradual underflow, and output range follow the common contract and NUM.

## Fixed reads, algorithmic control and schemas

For output q, read the complete B_k for every k in B_q, with worst-case H(2r) support; include the guide at q. The a/b S stages do not imply complete-input demand: exact support is the union of the two clipped windows. The guide is both Data and Control.

Boundary coordinates use the complete logical image; ROI and tile edges are not new image boundaries. Lowering closes mathematical sample support on fixed input member-tile grids. Data-dependent selection uses a finite static superset of every address the task may consume; runtime selection does not expand the Plan. Data and control are algorithmic inputs, with no separate Validation read set. Empty retained selections introduce no payload tasks. Output schemas derive only from static parameters and input schemas. Numerical, encoding and association premises remain unchecked under the common contract.

## Algorithm, resources, and execution

The direct reference has O(PAG³) work for fixed G≤4. Exact moment/prefix methods and certified linear solves may optimize it. Budget the Float64 a/b stages explicitly.

In complexity notation, P=HW, C is the number of independent planes, A is the tap count, and T is the iteration count. Arbitrary-precision limb costs are additional; these are not measured benchmarks. The shared contract governs work/memory/stage admission, cancellation, failure status, and owner lifetime. A failed call publishes no successful Result export; previously retired call exports remain valid, and final RET results require successful Run completion. Do not spill automatically, reduce precision silently, or fall back to retired implementations.

## Oracle, fixtures, and acceptance

With a constant guide, a=0 and the output is box(box(input)), not a single box. At radius zero, input values are copied bitwise, including NUM-defined exceptional values. Changing input samples at distances r..2r can affect q. Large offsets in covariance test cases must not create negative approximate variance.

The reference function evaluates the numerical formula on small inputs; it is neither a production kernel nor a complete port-schema/preflight simulator. ExactRational uses rational expressions with direct IEEE rounding. For transcendental functions, only DirectedMPFR results whose intervals close to one rounded value are strict goldens. Report Inconclusive when the interval does not determine rounding; do not substitute an approximation.

`fil07.spatial.guided(image, guide, radius_y, radius_x, *, epsilon, metric_scale, dtype)` → [source](../../../../oracle/ops/filter/fil07/spatial.py) .

Fixture mapping and evidence scope: see [oracle coverage](../oracle-coverage.md).

See the [oracle README](../../../../oracle/ops/filter/README.md) for reproduction and evidence levels. The listed self-checks cover only the reference program. Parameter boundaries, full rank/batch behavior, metadata, compiled ROI/tile selection, budgets, cancellation, and owners still require [runtime acceptance](FILTER_oracle_protocol.md). Passing an identity or constant fixture does not accept the full algorithm or all configurations.

## Backend and registration gates

The keys above are naming proposals only; strict and both CPU accelerated variants are not registered or implemented. Accelerated variants must satisfy NUM final FP32-scaled four-ULP bounds and fallback rules; error budgets cannot be accumulated across taps, axes, stages, or iterations. Thresholds, ordering, boundaries, copies, and output support that require exact selection cannot change approximately.

Float64 accelerated execution retains Float64 inputs/outputs and exponent range; it does not first convert to Float32. Values outside the Float32-scale range use strict NUM fallback. No kernel, CPU, or ISA performance or differential acceptance was performed here.

## Conceptual DAG (not an existing API)

`static parameters + schemas → compile fixed reads/tasks → prepare → unchecked arithmetic with declared RN stages → complete tiles → ordered call retirement`

This diagram introduces no implicit color conversion, hidden alpha premultiplication, automatic spectral correction, or zero-filling of missing data. complete-input demand and multi-stage buffers are governed by this member and its family contract.
