---
spec_schema_version: 1
id: FIL-13A
kind: authoring_helper
category: 05-filter
status: Proposed
document_maturity: D2_draft
implementation_status: not_implemented
parent_id: FIL-13
function: canny_quantized4
proposed_operation_keys: []
numeric_reference: S
oracle_scope: composed_math_references
research_sources:
- S06
---

# FIL-13A: canny_quantized4

Static Canny workflow. Status **Proposed / D2_draft**.

Inherits the [FIL-13 family contract](FIL-13_contract.md), [FILTER common contract](FILTER_common_contract.md), [NUM numerical/precision contract](../../01-numeric/op_specs/NUM_common_contract.md), and [FMT metadata/straight-semantics contract](../../02-format-color/op_specs/FMT_common_contract.md). The family contract and this member together form the complete draft. Do not override the RN, underflow, or accelerated-error baseline.

## Ports, shape, and semantics

`input` is a single-plane floating-point field → `edges` UInt8 with the same shape.

Except for semantic entry points explicitly defined by this member, inputs are raw numerical fields: channel count does not imply RGB/alpha, and attached color descriptions do not broaden sample-domain validation. Axes, positive extents, explicit broadcasting, canonical planar publication, and retention or reconstruction of applicable metadata follow the shared contract. Dependencies for components and multiple outputs are declared per request; associated collections follow the FilterBands/FrequencyGrid draft.

## Static parameters and valid domains

Gaussian sigma and radius are explicit; the gradient is fixed to FIL-08B unit_ramp with dx=dy=1; magnitude=l1|l2; 0<low<high; explicit connectivity=4|8; axes are explicit; every spatial stage uses boundary=reflect_half.

Exceptional values and floating-point overflow follow the corresponding NUM operations and the [FILTER common contract](FILTER_common_contract.md). Do not reject NaN/Inf or turn ordinary floating-point overflow into failure under a category-wide rule. Parameter domains, structure, integer overflow, resources, cancellation, and upstream errors remain governed by their contracts. Every parameter must explicitly state its type, valid domain, and value; constructors provide no implicit defaults. Floating-point parameters are interpreted using their actually stored values.

## Mathematical reference and rounding boundaries

Numerical class: **S**; see [FILTER_numeric_reference](FILTER_numeric_reference.md) for the E/B/S notation.

First apply FIL-04B, FIL-08B, and FIL-09A, preserving each member’s RN_t stage.NMS compares the original magnitude with neighbors in two directions: choose x when abs(gx)≥2abs(gy), choose y when abs(gy)≥2abs(gx), otherwise choose (1,1) or (1,-1) according to sign(gx*gy). Ties prefer the first, x-axis branch; a zero vector produces zero response. An out-of-bounds neighbor is treated as +Inf, suppressing the edge. Keep the center only when it is ≥ the positive-direction neighbor and > the negative-direction neighbor; otherwise set it to zero. Mark NMS values ≥low as weak and ≥high as strong, then apply B connectivity.

When a formula calls another member, preserve the RN stages produced at the call site as specified here or by the family contract; internal temporaries must not introduce undeclared rounding. Signed zero, dynamic-domain failures, gradual underflow, and output range follow the shared contract and NUM.

## Data / Control / Validation / Descriptor

The edge output is Whole for each plane. Gaussian and gradient stages remain local, but hysteresis reads the complete NMS plane. Batches do not read one another.

Boundary coordinates are defined relative to the complete logical image; ROI/tile edges do not become new boundaries. Empty demand reads no payload; selection conditions retain Control dirty witnesses. Exact support must not be replaced by a convenient rectangular or Whole read; any enlargement requires an explicit Conservative plan. Output descriptors derive from static parameters and input descriptors, not input sample values.

## Algorithm, resources, and execution

O(PA)+O(P); NMS and queue space are O(P). A scan-based union-find is possible but requires global coordination.

In complexity notation, P=HW, C is the number of independent planes, A the number of taps, and T the number of steps. Arbitrary-precision limb bit complexity is additional and is not a measured benchmark. The shared protocol defines work/memory/stage admission, cancellation, failure state, and owner lifetime. Failures do not publish partial Values or CompleteBundles. Do not spill automatically, silently lower precision, or call retired implementations as fallback.

## Oracle, fixtures, and acceptance

A constant input produces all zeros; a long weak chain connected to a strong pixel across multiple tiles is retained; plateau ties are deterministic on the same platform; low=0 is rejected; an ROI result matches the corresponding crop of Whole execution.

The functions below cover subformulas/stages of this construction; named workflow tests in tests.py cover only their recorded configurations. They are not Photospider DAG builders and do not claim complete metadata, Region, or resource integration. Stage RN boundaries must not disappear when nodes are fused.

`spatial.canny(image, *, sigma=1, radius=1, low=0.1, high=0.2, norm='l2', connectivity=8, dtype='float64')` → [source](../../../../oracle/ops/filter/oracles/spatial.py).
`spatial.hysteresis(weak, strong, connectivity=8)` → [source](../../../../oracle/ops/filter/oracles/spatial.py).

Fixture mapping and evidence scope: see [oracle coverage](../oracle-coverage.md).

See the [oracle README](../../../../oracle/ops/filter/README.md) for reproduction and evidence levels. The listed self-tests validate only the reference program; parameter boundaries, full rank/batch, metadata, ROI/dirty behavior, budgets, cancellation, and ownership still require [runtime acceptance](FILTER_oracle_protocol.md). Passing an identity/constant fixture does not accept the entire algorithm or every configuration.

## Backend and registration gates

This document proposes no native arithmetic key ready for registration. An authoring helper must explicitly connect finalized members; an external engine must first pass resource, licensing, real-golden, and numerical-configuration gates.

Float64 accelerated execution still uses Float64 inputs, outputs, and exponent range; it does not first convert to Float32. Values outside the Float32-scaled range use strict NUM fallback. No kernel/CPU/ISA performance or differential acceptance was run for this package.

## Conceptual DAG (not an existing API)

`static + descriptors → preflight / demand → Data and Control declared by this member → exact expression / declared RN stages → requested owned output / complete result`

This sketch adds no implicit color conversion, hidden alpha premultiplication, automatic spectral correction, or zero-filling of missing data. Whole execution and multistage buffers follow this member and its family contract.

## Rationale and open decisions

[S06 · scikit-image feature](../research-sources.md#s06)

External sources provide algorithmic or definitional background. The finite windows, rounding, ties, units, defaults, and execution profile here are explicit project choices and do not claim bitwise identity with any library or commercial software.
