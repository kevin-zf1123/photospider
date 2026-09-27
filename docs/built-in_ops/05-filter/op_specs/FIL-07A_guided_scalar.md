---
spec_schema_version: 1
id: FIL-07A
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D2_draft
implementation_status: not_implemented
parent_id: FIL-07
function: guided_scalar
proposed_operation_keys:
- filter.guided_scalar_strict
- filter.guided_scalar_accelerated_apple_silicon
- filter.guided_scalar_accelerated_x86_64
numeric_reference: 'S: coefficients RN64; final E'
oracle_scope: mathematical_reference
---

# FIL-07A: guided_scalar

Scalar guided filter. Status **Proposed / D2_draft**; legacy test kernels and their behavior are not compatibility targets.

Inherits [FIL-07 family contract](FIL-07_contract.md) and [FILTER common contract](FILTER_common_contract.md) and [NUM numerical/accuracy contract](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT metadata/straight semantics](../../02-format-color/op_specs/FMT_common_contract.md). The family and member specifications together form the complete draft; do not override their RN, underflow, or accelerated-error requirements.

## Ports, shape, and semantics

`input,guide` → `output`; `input` has C independent planes, and both inputs match in H/W and batch dimensions. FIL-07A requires a one-component guide; FIL-07B requires a guide with 1..4 components and an explicit `component_axis` distinct from the spatial axes.

Unless this member defines a semantic entry point, inputs are raw numerical fields: channel count does not imply RGB/alpha, and attached color descriptions do not widen sample-domain validation. Axes, positive extents, explicit broadcasting, canonical planar publication, and preservation or reconstruction of applicable metadata follow the common contract. Components and multiple outputs declare dependencies per request; associated collections follow the proposed FilterBands/FrequencyGrid contracts.

## Static parameters and valid domains

`radius_y` and `radius_x` are nonnegative Int64. `epsilon` is a positive finite Float64 in squared units of the metric-scaled guide. `guide_metric_scale` is an explicitly supplied positive finite Float64 scalar or vector. The boundary profile is fixed to clipped/truncate two-window support.

Exceptional values and floating-point overflow follow the corresponding NUM operations and the [FILTER common contract](FILTER_common_contract.md). Do not reject NaN/Inf under a category-wide rule or convert ordinary floating-point overflow into failure. Explicit parameter-domain, structural, integer-overflow, resource, cancellation, and upstream errors remain governed by their respective contracts. Every static parameter must be explicitly supplied with its type and valid domain; constructors have no implicit defaults. Floating-point parameters use their stored values.

## Mathematical reference and rounding boundaries

Numerical classification: **S: coefficients RN64; final E**. See [FILTER_numeric_reference](FILTER_numeric_reference.md) for E/B/S.

Let B_k be the image-clipped box centered at k, and let g be the exactly scaled guide. Evaluate exactly `mu_g,mu_p,Cgg=mean(ggᵀ)-mu_g mu_gᵀ,Cgp=mean(gp)-mu_g mu_p` .`a_k=RN64_vec((Cgg+epsilon I)^-1 Cgp)` after an exact matrix solve, rounding each component once to RN64; `b_k=RN64(mu_p-dot(a_k,mu_g))` (using the already rounded a). Output `RN_t(dot(mean_{k∈Bq}(a_k),g_q)+mean(b_k))`; both means are exact. The vector SPD solution is unique and uses no arbitrary epsilon jitter; the scalar case is the same 1×1 formula.

If the formula invokes another member, preserve the RN stages at the call site as specified here or by the family contract. Do not introduce undeclared rounding in internal temporary calculations. Signed zero, dynamic-domain failures, gradual underflow, and output range follow the common contract and NUM.

## Data / Control / Validation / Descriptor

For output q, read the complete B_k for every k in B_q, with worst-case H(2r) support; include the guide at q. The a/b S stages do not imply Whole demand: exact support is the union of the two clipped windows. The guide is both Data and Control.

Boundary coordinates are defined against the complete logical image; ROI/tile edges do not become new boundaries. An empty demand reads no payload. Selection conditions retain Control dirty witnesses. Exact support cannot be replaced by a convenient rectangular or Whole read; any expansion must be explicitly requested as a Conservative plan. Output descriptors are inferred from static parameters and input descriptors, not sample values.

## Algorithm, resources, and execution

The direct reference has O(PAG³) work for fixed G≤4. Exact moment/prefix methods and certified linear solves may optimize it. Budget the Float64 a/b stages explicitly.

In complexity notation, P=HW, C is the number of independent planes, A is the tap count, and T is the iteration count. Arbitrary-precision limb costs are additional; these are not measured benchmarks. The shared contract governs work/memory/stage admission, cancellation, failure status, and owner lifetime. Failures do not publish partial Values or CompleteBundles. Do not spill automatically, reduce precision silently, or fall back to retired implementations.

## Oracle, fixtures, and acceptance

With a constant guide, a=0 and the output is box(box(input)), not a single box. At radius zero, input values are copied bitwise, including NUM-defined exceptional values. Changing input samples at distances r..2r can affect q. Large offsets in covariance test cases must not create negative approximate variance.

The reference function evaluates the numerical formula on small inputs; it is neither a production kernel nor a complete port-schema/preflight simulator. ExactRational uses rational expressions with direct IEEE rounding. For transcendental functions, only DirectedMPFR results whose intervals close to one rounded value are strict goldens. Report Inconclusive when the interval does not determine rounding; do not substitute an approximation.

`spatial.guided(image, guide, radius_y, radius_x, *, epsilon, metric_scale, dtype)` → [source](../../../../oracle/ops/filter/oracles/spatial.py) .

Fixture mapping and evidence scope: see [oracle coverage](../oracle-coverage.md).

See the [oracle README](../../../../oracle/ops/filter/README.md) for reproduction and evidence levels. The listed self-checks cover only the reference program. Parameter boundaries, full rank/batch behavior, metadata, ROI/dirty propagation, budgets, cancellation, and owners still require [runtime acceptance](FILTER_oracle_protocol.md). Passing an identity or constant fixture does not accept the full algorithm or all configurations.

## Backend and registration gates

The keys above are naming proposals only; strict and both CPU accelerated variants are not registered or implemented. Accelerated variants must satisfy NUM final FP32-scaled four-ULP bounds and fallback rules; error budgets cannot be accumulated across taps, axes, stages, or iterations. Thresholds, ordering, boundaries, copies, and output support that require exact selection cannot change approximately.

Float64 accelerated execution retains Float64 inputs/outputs and exponent range; it does not first convert to Float32. Values outside the Float32-scale range use strict NUM fallback. No kernel, CPU, or ISA performance or differential acceptance was performed here.

## Conceptual DAG (not an existing API)

`static + descriptors → preflight / demand → Data and Control declared by this member → exact expression / declared RN stages → requested owned output / complete result`

This diagram introduces no implicit color conversion, hidden alpha premultiplication, automatic spectral correction, or zero-filling of missing data. Whole demand and multi-stage buffers are governed by this member and its family contract.
