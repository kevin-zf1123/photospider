---
spec_schema_version: 1
id: FIL-14C
kind: authoring_helper
category: 05-filter
status: Proposed
document_maturity: D2_draft
implementation_status: not_implemented
parent_id: FIL-14
function: gabor_bank
proposed_operation_keys: []
numeric_reference: S
oracle_scope: composed_math_references
research_sources:
- S01
- S26
---

# FIL-14C: gabor_bank

Named Gabor filter bank. Status **Proposed / D2_draft**; the legacy test kernel and its behavior are not compatibility targets.

Inherits the [FIL-14 family contract](FIL-14_contract.md), [FILTER common contract](FILTER_common_contract.md), [NUM numerical/precision contract](../../01-numeric/op_specs/NUM_common_contract.md), and [FMT metadata/straight-semantics contract](../../02-format-color/op_specs/FMT_common_contract.md). The family contract and this member together form the complete draft. Do not override the RN, underflow, or accelerated-error baseline.

## Ports, shape, and semantics

`input` plus a static list of configurations → FilterBands/v1 `responses`; each bank_id has real and imaginary roles with the input shape.

Except for semantic entry points explicitly defined by this member, inputs are raw numerical fields: channel count does not imply RGB/alpha, and attached color descriptions do not broaden sample-domain validation. Axes, positive extents, explicit broadcasting, canonical planar publication, and retention or reconstruction of applicable metadata follow the shared contract. Dependencies for components and multiple outputs are declared per request; associated collections follow the FilterBands/FrequencyGrid draft.

## Static parameters and valid domains

Require at least one unique bank_id with explicit ordering. Each entry has FIL-14A parameters and FIL-14B boundaries; frequency is never selected automatically from data.

Exceptional values and floating-point overflow follow the corresponding NUM operations and the [FILTER common contract](FILTER_common_contract.md). Do not reject NaN/Inf or turn ordinary floating-point overflow into failure under a category-wide rule. Parameter domains, structure, integer overflow, resources, cancellation, and upstream errors remain governed by their contracts. Every parameter must explicitly state its type, valid domain, and value; constructors provide no implicit defaults. Floating-point parameters are interpreted using their actually stored values.

## Mathematical reference and rounding boundaries

Numerical class: **S**; see [FILTER_numeric_reference](FILTER_numeric_reference.md) for the E/B/S notation.

Compose FIL-14A→FIL-14B for each entry with S/DAG semantics. Bank outputs are not summed; normalization is recorded with each kernel.

When a formula calls another member, preserve the RN stages produced at the call site as specified here or by the family contract; internal temporaries must not introduce undeclared rounding. Signed zero, dynamic-domain failures, gradual underflow, and output range follow the shared contract and NUM.

## Data / Control / Validation / Descriptor

Collection association descriptors are validated for the entire configuration list. Payload execution is lazy by bank: evaluate only the requested bank and its required inputs.

Boundary coordinates are defined relative to the complete logical image; ROI/tile edges do not become new boundaries. Empty demand reads no payload; selection conditions retain Control dirty witnesses. Exact support must not be replaced by a convenient rectangular or Whole read; any enlargement requires an explicit Conservative plan. Output descriptors derive from static parameters and input descriptors, not input sample values.

## Algorithm, resources, and execution

O(BPCA). Work may be batched, but CompleteBundle associations must be complete and fit within budget.

In complexity notation, P=HW, C is the number of independent planes, A the number of taps, and T the number of steps. Arbitrary-precision limb bit complexity is additional and is not a measured benchmark. The shared protocol defines work/memory/stage admission, cancellation, failure state, and owner lifetime. Failures do not publish partial Values or CompleteBundles. Do not spill automatically, silently lower precision, or call retired implementations as fallback.

## Oracle, fixtures, and acceptance

A single bank matches FIL-14B. Duplicate parameters are allowed under different IDs; duplicate IDs are rejected. Changing order changes only collection order.

The functions below cover subformulas/stages of this construction; named workflow tests in tests.py cover only their recorded configurations. They are not Photospider DAG builders and do not claim complete metadata, Region, or resource integration. Stage RN boundaries must not disappear when nodes are fused.

`transcend.gabor_kernel(*, frequency, theta=0, phase=0, sigma=1, aspect=1, radius_y=1, radius_x=1, dc='keep', normalization='none')` → [source](../../../../oracle/ops/filter/oracles/transcend.py).
`spatial.convolve2d(image, kernel, anchor=(0, 0), *, direction='correlate', normalization='none', bias=0, boundary='reflect_half', cval=0, output_shape='same', dtype='float64')` → [source](../../../../oracle/ops/filter/oracles/spatial.py).

Fixture mapping and evidence scope: see [oracle coverage](../oracle-coverage.md).

See the [oracle README](../../../../oracle/ops/filter/README.md) for reproduction and evidence levels. The listed self-tests validate only the reference program; parameter boundaries, full rank/batch, metadata, ROI/dirty behavior, budgets, cancellation, and ownership still require [runtime acceptance](FILTER_oracle_protocol.md). Passing an identity/constant fixture does not accept the entire algorithm or every configuration.

## Backend and registration gates

This document proposes no native arithmetic key ready for registration. An authoring helper must explicitly connect finalized members; an external engine must first pass resource, licensing, real-golden, and numerical-configuration gates.

Float64 accelerated execution still uses Float64 inputs, outputs, and exponent range; it does not first convert to Float32. Values outside the Float32-scaled range use strict NUM fallback. No kernel/CPU/ISA performance or differential acceptance was run for this package.

## Conceptual DAG (not an existing API)

`static + descriptors → preflight / demand → Data and Control declared by this member → exact expression / declared RN stages → requested owned output / complete result`

This sketch adds no implicit color conversion, hidden alpha premultiplication, automatic spectral correction, or zero-filling of missing data. Whole execution and multistage buffers follow this member and its family contract.

## Rationale and open decisions

[S01 · OpenCV Image Filtering](../research-sources.md#s01)；[S26 · scikit-image filters](../research-sources.md#s26)

External sources provide algorithmic or definitional background. The finite windows, rounding, ties, units, defaults, and execution profile here are explicit project choices and do not claim bitwise identity with any library or commercial software.
