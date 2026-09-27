---
spec_schema_version: 1
id: FIL-12B
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
parent_id: FIL-12
function: structure_tensor2d
proposed_operation_keys:
- filter.structure_tensor2d_strict
- filter.structure_tensor2d_accelerated_apple_silicon
- filter.structure_tensor2d_accelerated_x86_64
numeric_reference: E
oracle_scope: mathematical_reference
---

# FIL-12B: structure_tensor2d

Smoothed gradient outer-product tensor. Status **Proposed / D1_draft**; legacy test kernels and their behavior are not compatibility targets.

Inherits the [FIL-12 family contract](FIL-12_contract.md), [FILTER common contract](FILTER_common_contract.md), [NUM numerical/accuracy contract](../../01-numeric/op_specs/NUM_common_contract.md), and [FMT metadata/straight semantics](../../02-format-color/op_specs/FMT_common_contract.md). The family and member specifications together form the complete draft; do not override their RN, underflow, or accelerated-error requirements.

## Ports, shape, and semantics

`gx,gy` → independently requestable `jxx,jxy,jyy` outputs.

Unless this member defines a semantic entry point, inputs are raw numerical fields: channel count does not imply RGB/alpha, and attached color descriptions do not widen sample-domain validation. Axes, positive extents, explicit broadcasting, canonical planar publication, and preservation or reconstruction of applicable metadata follow the common contract. Components and multiple outputs declare dependencies per request; associated collections follow the proposed FilterBands/FrequencyGrid contracts.

## Static parameters and valid domains

Explicit Int64 box height, width, and `anchor_y,anchor_x`, including for odd-sized boxes; fixed `reflect_half` boundary with `cval=0`. This operation does not compute derivatives; weights are the valid-box mean.

Exceptional values and floating-point overflow follow the corresponding NUM operations and the [FILTER common contract](FILTER_common_contract.md). Do not reject NaN/Inf under a category-wide rule or convert ordinary floating-point overflow into failure. Explicit parameter-domain, structural, integer-overflow, resource, cancellation, and upstream errors remain governed by their respective contracts. Every static parameter must be explicitly supplied with its type and valid domain; constructors have no implicit defaults. Floating-point parameters use their stored values.

## Mathematical reference and rounding boundaries

Numerical classification: **E**. See [FILTER_numeric_reference](FILTER_numeric_reference.md) for E/B/S.

`jxx=RN_t(mean(gx²))`, `jxy=RN_t(mean(gx*gy))`, and analogously for jyy. Do not first materialize rounded square/product tensors. Explicit NUM multiplication followed by box filtering is a different S chain.

If the formula invokes another member, preserve the RN stages at the call site as specified here or by the family contract. Do not introduce undeclared rounding in internal temporary calculations. Signed zero, dynamic-domain failures, gradual underflow, and output range follow the common contract and NUM.

## Data / Control / Validation / Descriptor

`jxx` reads only the `gx` footprint; `jyy` only the `gy` footprint; `jxy` reads both.

Boundary coordinates are defined against the complete logical image; ROI/tile edges do not become new boundaries. An empty demand reads no payload. Selection conditions retain Control dirty witnesses. Exact support cannot be replaced by a convenient rectangular or Whole read; any expansion must be explicitly requested as a Conservative plan. Output descriptors are inferred from static parameters and input descriptors, not sample values.

## Algorithm, resources, and execution

O(PCA) exact reference work; exact sliding moments may optimize it.

In complexity notation, P=HW, C is the number of independent planes, A is the tap count, and T is the iteration count. Arbitrary-precision limb costs are additional; these are not measured benchmarks. The shared contract governs work/memory/stage admission, cancellation, failure status, and owner lifetime. Failures do not publish partial Values or CompleteBundles. Do not spill automatically, reduce precision silently, or fall back to retired implementations.

## Oracle, fixtures, and acceptance

For constant gx=2, gy=3, outputs are 4, 6, and 9. The exact matrix is PSD; do not silently clip tiny negative eigenvalues that appear after elementwise rounding.

The reference function evaluates the numerical formula on small inputs; it is neither a production kernel nor a complete port-schema/preflight simulator. ExactRational uses rational expressions with direct IEEE rounding. For transcendental functions, only DirectedMPFR results whose intervals close to one rounded value are strict goldens. Report Inconclusive when the interval does not determine rounding; do not substitute an approximation.

`spatial.structure_tensor(gx, gy, height, width, anchor, *, boundary, cval, dtype)` → [source](../../../../oracle/ops/filter/oracles/spatial.py).

Fixture mapping and evidence scope: see [oracle coverage](../oracle-coverage.md).

See the [oracle README](../../../../oracle/ops/filter/README.md) for reproduction and evidence levels. The listed self-checks cover only the reference program. Parameter boundaries, full rank/batch behavior, metadata, ROI/dirty propagation, budgets, cancellation, and owners still require [runtime acceptance](FILTER_oracle_protocol.md). Passing an identity or constant fixture does not accept the full algorithm or all configurations.

## Backend and registration gates

The keys above are naming proposals only; strict and both CPU accelerated variants are not registered or implemented. Accelerated variants must satisfy NUM final FP32-scaled four-ULP bounds and fallback rules; error budgets cannot be accumulated across taps, axes, stages, or iterations. Thresholds, ordering, boundaries, copies, and output support that require exact selection cannot change approximately.

Float64 accelerated execution retains Float64 inputs/outputs and exponent range; it does not first convert to Float32. Values outside the Float32-scale range use strict NUM fallback. No kernel, CPU, or ISA performance or differential acceptance was performed here.

## Conceptual DAG (not an existing API)

`static + descriptors → preflight / demand → Data and Control declared by this member → exact expression / declared RN stages → requested owned output / complete result`

This diagram introduces no implicit color conversion, hidden alpha premultiplication, automatic spectral correction, or zero-filling of missing data. Whole demand and multi-stage buffers are governed by this member and its family contract.
