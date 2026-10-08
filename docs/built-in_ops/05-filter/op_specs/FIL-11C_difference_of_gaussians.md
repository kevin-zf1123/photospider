---
spec_schema_version: 1
id: FIL-11C
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
parent_id: FIL-11
function: difference_of_gaussians
proposed_operation_keys:
- filter.difference_of_gaussians_strict
- filter.difference_of_gaussians_accelerated_apple_silicon
- filter.difference_of_gaussians_accelerated_x86_64
numeric_reference: S
oracle_scope: mathematical_reference
---

# FIL-11C: difference_of_gaussians

Two-scale Gaussian difference. Status **Proposed / D1_draft**.

Inherits the [FIL-11 family contract](FIL-11_contract.md), [FILTER common contract](FILTER_common_contract.md), [NUM numerical/accuracy contract](../../01-numeric/op_specs/NUM_common_contract.md), and [FMT metadata/straight semantics](../../02-format-color/op_specs/FMT_common_contract.md). The family and member specifications together form the complete draft; do not override their RN, underflow, or accelerated-error requirements.

## Ports, shape, and semantics

`input` → `output`.

Unless this member defines a semantic entry point, inputs are raw numerical fields: channel count does not imply RGB/alpha, and attached color descriptions do not widen sample-domain validation. Axes, positive extents, explicit broadcasting, canonical planar publication, and preservation or reconstruction of applicable metadata follow the common contract. Components and multiple outputs declare dependencies per request; associated collections follow the proposed FilterBands/FrequencyGrid contracts.

## Static parameters and valid domains

Explicit positive finite Float64 `sigma_small`; explicit finite Float64 `ratio>1`; explicitly supplied Int64 radii for both Gaussian filters; explicit Int64 spatial axes; fixed `reflect_half` boundary and `cval=0`; explicit finite Float64 `gain`. No constructor defaults.

Exceptional values and floating-point overflow follow the corresponding NUM operations and the [FILTER common contract](FILTER_common_contract.md). Do not reject NaN/Inf under a category-wide rule or convert ordinary floating-point overflow into failure. Explicit parameter-domain, structural, integer-overflow, resource, cancellation, and upstream errors remain governed by their respective contracts. Every static parameter must be explicitly supplied with its type and valid domain; constructors have no implicit defaults. Floating-point parameters use their stored values.

## Mathematical reference and rounding boundaries

Numerical classification: **S**. See [FILTER_numeric_reference](FILTER_numeric_reference.md) for E/B/S.

Derive the larger sigma at the explicit rounding point `RN64(sigma_small*ratio)`. The two FIL-04B outputs are each rounded to RN_t; the final output is `RN_t(gain*(Gsmall-Glarge))`. This S profile rounds both Gaussian stages applied to the same input and does not claim equivalence to LoG.

If the formula invokes another member, preserve the RN stages at the call site as specified here or by the family contract. Do not introduce undeclared rounding in internal temporary calculations. Signed zero, dynamic-domain failures, gradual underflow, and output range follow the common contract and NUM.

## Data / Control / Validation / Descriptor

Support is the union of the two Gaussian footprints. The two rounded stages may be cached independently; the final stage at q reads their corresponding rounded values.

Boundary coordinates are defined against the complete logical image; ROI/tile edges do not become new boundaries. An empty demand reads no payload. Selection conditions retain Control dirty witnesses. Exact support cannot be replaced by a convenient rectangular or Whole read; any expansion must be explicitly requested as a Conservative plan. Output descriptors are inferred from static parameters and input descriptors, not sample values.

## Algorithm, resources, and execution

Two blur stages plus O(PC) pointwise work.

In complexity notation, P=HW, C is the number of independent planes, A is the tap count, and T is the iteration count. Arbitrary-precision limb costs are additional; these are not measured benchmarks. The shared contract governs work/memory/stage admission, cancellation, failure status, and owner lifetime. Failures do not publish partial Values or CompleteBundles. Do not spill automatically, reduce precision silently, or fall back to retired implementations.

## Oracle, fixtures, and acceptance

A constant input yields zero. An invalid ratio fails preflight. Include a fixture sensitive to intermediate rounding when Gsmall and Glarge are nearly equal.

The reference function evaluates the numerical formula on small inputs; it is neither a production kernel nor a complete port-schema/preflight simulator. ExactRational uses rational expressions with direct IEEE rounding. For transcendental functions, only DirectedMPFR results whose intervals close to one rounded value are strict goldens. Report Inconclusive when the interval does not determine rounding; do not substitute an approximation.

`transcend.gaussian_kernel(sigma, radius)` → [source](../../../../oracle/ops/filter/oracles/transcend.py).
`spatial.separable(image, kx, ky, anchor, **kwargs)` → [source](../../../../oracle/ops/filter/oracles/spatial.py).

Fixture mapping and evidence scope: see [oracle coverage](../oracle-coverage.md).

See the [oracle README](../../../../oracle/ops/filter/README.md) for reproduction and evidence levels. The listed self-checks cover only the reference program. Parameter boundaries, full rank/batch behavior, metadata, ROI/dirty propagation, budgets, cancellation, and owners still require [runtime acceptance](FILTER_oracle_protocol.md). Passing an identity or constant fixture does not accept the full algorithm or all configurations.

## Backend and registration gates

The keys above are naming proposals only; strict and both CPU accelerated variants are not registered or implemented. Accelerated variants must satisfy NUM final FP32-scaled four-ULP bounds and fallback rules; error budgets cannot be accumulated across taps, axes, stages, or iterations. Thresholds, ordering, boundaries, copies, and output support that require exact selection cannot change approximately.

Float64 accelerated execution retains Float64 inputs/outputs and exponent range; it does not first convert to Float32. Values outside the Float32-scale range use strict NUM fallback. No kernel, CPU, or ISA performance or differential acceptance was performed here.

## Conceptual DAG (not an existing API)

`static + descriptors → preflight / demand → Data and Control declared by this member → exact expression / declared RN stages → requested owned output / complete result`

This diagram introduces no implicit color conversion, hidden alpha premultiplication, automatic spectral correction, or zero-filling of missing data. Whole demand and multi-stage buffers are governed by this member and its family contract.
