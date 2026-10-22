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

Named Gabor filter bank. Status **Proposed / D2_draft**.

Inherits the [FIL-14 family contract](FIL-14_contract.md), [FILTER common contract](FILTER_common_contract.md), [NUM numerical/precision contract](../../01-numeric/op_specs/NUM_common_contract.md), and [FMT metadata/straight-semantics contract](../../02-format-color/op_specs/FMT_common_contract.md). The family contract and this member together form the complete draft. Do not override the RN, underflow, or accelerated-error baseline.

## Ports, shape, and semantics

`input` plus a static list of configurations → FilterBands/v1 `responses`; each bank_id has real and imaginary roles with the input shape.

Ports carry Results with fixed Tensor member schemas. Unless the member defines an explicit color interpretation, samples are raw numerical values; channel count does not imply RGB or alpha. Color interpretation is explicit configuration. Positive extents, axes and explicit broadcasting are checked during static construction. Materialized output tiles have independent allocations. FilterBands and FrequencyGrid are proposed SDK/workflow representations under the collection contract; lowering records each selected output's fixed reads and writes.

## Static parameters and valid domains

Require at least one unique bank_id with explicit ordering. Each entry has FIL-14A parameters and FIL-14B boundaries; frequency is never selected automatically from data.

Exceptional values and floating-point overflow follow the corresponding NUM operations and the [FILTER common contract](FILTER_common_contract.md). Do not reject NaN/Inf or turn ordinary floating-point overflow into failure under a category-wide rule. Static structural, parameter-type and capacity-arithmetic errors retain pre-execution diagnostics. Numerical-domain premises are unchecked; resource, cancellation and upstream failures retain their contracts. Every parameter must explicitly state its type, valid domain, and value; constructors provide no implicit defaults. Floating-point parameters are interpreted using their actually stored values.

## Mathematical reference and rounding boundaries

Numerical class: **S**; see [FILTER_numeric_reference](FILTER_numeric_reference.md) for the E/B/S notation.

Compose FIL-14A→FIL-14B for each entry with S/DAG semantics. Bank outputs are not summed; normalization is recorded with each kernel.

When a formula calls another member, preserve the RN stages produced at the call site as specified here or by the family contract; internal temporaries must not introduce undeclared rounding. Signed zero, numerical-domain premises and explicit valid-input no-solution outcomes, gradual underflow, and output range follow the shared contract and NUM.

## Fixed reads, algorithmic control and schemas

The complete static bank directory and output schemas are checked during compilation. Lowering instantiates only selected banks and their required inputs. Runtime payload associations are caller premises; published Results have fixed selected tiles and do not acquire additional banks later.

Boundary coordinates use the complete logical image; ROI and tile edges are not new image boundaries. Lowering closes mathematical sample support on fixed input member-tile grids. Data-dependent selection uses a finite static superset of every address the task may consume; runtime selection does not expand the Plan. Data and control are algorithmic inputs, with no separate Validation read set. Empty retained selections introduce no payload tasks. Output schemas derive only from static parameters and input schemas. Numerical, encoding and association premises remain unchecked under the common contract.

## Algorithm, resources, and execution

O(BPCA). Work may be batched through explicit tasks, while each selected output retains its fixed schema and independent tile allocations. Directory metadata, shared intermediates and output storage must fit within the declared resource bounds; call retirement governs external publication.

In complexity notation, P=HW, C is the number of independent planes, A the number of taps, and T the number of steps. Arbitrary-precision limb bit complexity is additional and is not a measured benchmark. The shared protocol defines work/memory/stage admission, cancellation, failure state, and owner lifetime. A failed call publishes no successful Result export; previously retired call exports remain valid, and final RET results require successful Run completion. Do not spill automatically, silently lower precision, or call retired implementations as fallback.

## Oracle, fixtures, and acceptance

A single bank matches FIL-14B. Duplicate parameters are allowed under different IDs; duplicate IDs are rejected. Changing order changes only collection order.

The functions below cover subformulas/stages of this construction; named workflow tests in tests.py cover only their recorded configurations. They are not Photospider DAG builders and do not claim complete metadata, Region, or resource integration. Stage RN boundaries must not disappear when nodes are fused.

`fil14.transcend.gabor_kernel(*, frequency, theta=0, phase=0, sigma=1, aspect=1, radius_y=1, radius_x=1, dc='keep', normalization='none')` → [source](../../../../oracle/ops/filter/fil14/transcend.py).
`fil01.spatial.convolve2d(image, kernel, anchor=(0, 0), *, direction='correlate', normalization='none', bias=0, boundary='reflect_half', cval=0, output_shape='same', dtype='float64')` → [source](../../../../oracle/ops/filter/fil01/spatial.py).

Fixture mapping and evidence scope: see [oracle coverage](../oracle-coverage.md).

See the [oracle README](../../../../oracle/ops/filter/README.md) for reproduction and evidence levels. The listed self-tests validate only the reference program; parameter boundaries, full rank/batch, metadata, compiled ROI/tile behavior, budgets, cancellation, and ownership still require [runtime acceptance](FILTER_oracle_protocol.md). Passing an identity/constant fixture does not accept the entire algorithm or every configuration.

## Backend and registration gates

This document proposes no native arithmetic key ready for registration. An authoring helper must explicitly connect finalized members; an external engine must first pass resource, licensing, real-golden, and numerical-configuration gates.

Float64 accelerated execution still uses Float64 inputs, outputs, and exponent range; it does not first convert to Float32. Values outside the Float32-scaled range use strict NUM fallback. No kernel/CPU/ISA performance or differential acceptance was run for this package.

## Conceptual DAG (not an existing API)

`static parameters + schemas → compile fixed reads/tasks → prepare → unchecked arithmetic with declared RN stages → complete tiles → ordered call retirement`

This sketch preserves explicit color interpretation, alpha arithmetic, spectral formulas and missing-coverage semantics. Computation scope and multistage storage follow this member and its family contract; shared work is represented by explicit tasks and private SSA outputs.

## Rationale and open decisions

[S01 · OpenCV Image Filtering](../research-sources.md#s01)；[S26 · scikit-image filters](../research-sources.md#s26)

External sources provide algorithmic or definitional background. The finite windows, rounding, ties, units, defaults, and execution profile here are explicit project choices and do not claim bitwise identity with any library or commercial software.
