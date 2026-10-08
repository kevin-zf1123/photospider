---
spec_schema_version: 1
id: FIL-09B
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
parent_id: FIL-09
function: gradient_orientation
proposed_operation_keys:
- filter.gradient_orientation_strict
- filter.gradient_orientation_accelerated_apple_silicon
- filter.gradient_orientation_accelerated_x86_64
numeric_reference: E
oracle_scope: mathematical_reference
---

# FIL-09B: gradient_orientation

Gradient orientation and validity. Status **Proposed / D1_draft**.

Inherits the [FIL-09 family contract](FIL-09_contract.md), [FILTER common contract](FILTER_common_contract.md), [NUM numerical/accuracy contract](../../01-numeric/op_specs/NUM_common_contract.md), and [FMT metadata/straight semantics](../../02-format-color/op_specs/FMT_common_contract.md). The family and member specifications together form the complete draft; do not override their RN, underflow, or accelerated-error requirements.

## Ports, shape, and semantics

`gx,gy` → `angle,valid`; `angle` has the input dtype and shape; `valid` is UInt8 with the same shape.

Unless this member defines a semantic entry point, inputs are raw numerical fields: channel count does not imply RGB/alpha, and attached color descriptions do not widen sample-domain validation. Axes, positive extents, explicit broadcasting, canonical planar publication, and preservation or reconstruction of applicable metadata follow the common contract. Components and multiple outputs declare dependencies per request; associated collections follow the proposed FilterBands/FrequencyGrid contracts.

## Static parameters and valid domains

`unit` is explicitly supplied as `radian` or `turn`; output ranges are (-π,π] and (-1/2,1/2], respectively. No constructor default.

Exceptional values and floating-point overflow follow the corresponding NUM operations and the [FILTER common contract](FILTER_common_contract.md). Do not reject NaN/Inf under a category-wide rule or convert ordinary floating-point overflow into failure. Explicit parameter-domain, structural, integer-overflow, resource, cancellation, and upstream errors remain governed by their respective contracts. Every static parameter must be explicitly supplied with its type and valid domain; constructors have no implicit defaults. Floating-point parameters use their stored values.

## Mathematical reference and rounding boundaries

Numerical classification: **E**. See [FILTER_numeric_reference](FILTER_numeric_reference.md) for E/B/S.

For a nonzero vector, compute `atan2(gy,gx)`; map the negative x axis to +π. If both numeric inputs are zero, return angle +0 and valid=0; otherwise valid=1. In `turn` mode use `RN_t(angle_exact/(2π))`, not a rounded angle divided by a constant. Exceptional inputs and atan2 results follow NUM; a NaN-controlling validity comparison yields false validity and a propagated NaN angle.

If the formula invokes another member, preserve the RN stages at the call site as specified here or by the family contract. Do not introduce undeclared rounding in internal temporary calculations. Signed zero, dynamic-domain failures, gradual underflow, and output range follow the common contract and NUM.

## Data / Control / Validation / Descriptor

Read `gx` and `gy` pointwise. A validity-only request need not evaluate atan2, but must inspect both values to determine the zero-vector branch; it does not add a finite-only restriction.

Boundary coordinates are defined against the complete logical image; ROI/tile edges do not become new boundaries. An empty demand reads no payload. Selection conditions retain Control dirty witnesses. Exact support cannot be replaced by a convenient rectangular or Whole read; any expansion must be explicitly requested as a Conservative plan. Output descriptors are inferred from static parameters and input descriptors, not sample values.

## Algorithm, resources, and execution

O(PC) work with a directed atan2 enclosure and exact quadrant branches.

In complexity notation, P=HW, C is the number of independent planes, A is the tap count, and T is the iteration count. Arbitrary-precision limb costs are additional; these are not measured benchmarks. The shared contract governs work/memory/stage admission, cancellation, failure status, and owner lifetime. Failures do not publish partial Values or CompleteBundles. Do not spill automatically, reduce precision silently, or fall back to retired implementations.

## Oracle, fixtures, and acceptance

In input order `(gx,gy)=(0,1)` yields π/2; `(-1,±0)` yields +π; the zero vector is invalid. Preserve the distinction on either side of the negative x axis for tiny signed gy.

The reference function evaluates the numerical formula on small inputs; it is neither a production kernel nor a complete port-schema/preflight simulator. ExactRational uses rational expressions with direct IEEE rounding. For transcendental functions, only DirectedMPFR results whose intervals close to one rounded value are strict goldens. Report Inconclusive when the interval does not determine rounding; do not substitute an approximation.

`transcend.orientation(gx, gy, *, unit, dtype)` → [source](../../../../oracle/ops/filter/oracles/transcend.py).

Fixture mapping and evidence scope: see [oracle coverage](../oracle-coverage.md).

See the [oracle README](../../../../oracle/ops/filter/README.md) for reproduction and evidence levels. The listed self-checks cover only the reference program. Parameter boundaries, full rank/batch behavior, metadata, ROI/dirty propagation, budgets, cancellation, and owners still require [runtime acceptance](FILTER_oracle_protocol.md). Passing an identity or constant fixture does not accept the full algorithm or all configurations.

## Backend and registration gates

The keys above are naming proposals only; strict and both CPU accelerated variants are not registered or implemented. Accelerated variants must satisfy NUM final FP32-scaled four-ULP bounds and fallback rules; error budgets cannot be accumulated across taps, axes, stages, or iterations. Thresholds, ordering, boundaries, copies, and output support that require exact selection cannot change approximately.

Float64 accelerated execution retains Float64 inputs/outputs and exponent range; it does not first convert to Float32. Values outside the Float32-scale range use strict NUM fallback. No kernel, CPU, or ISA performance or differential acceptance was performed here.

## Conceptual DAG (not an existing API)

`static + descriptors → preflight / demand → Data and Control declared by this member → exact expression / declared RN stages → requested owned output / complete result`

This diagram introduces no implicit color conversion, hidden alpha premultiplication, automatic spectral correction, or zero-filling of missing data. Whole demand and multi-stage buffers are governed by this member and its family contract.
