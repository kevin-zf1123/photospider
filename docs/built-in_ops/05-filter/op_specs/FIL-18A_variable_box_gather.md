---
spec_schema_version: 1
id: FIL-18A
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
parent_id: FIL-18
function: variable_box_gather
proposed_operation_keys:
- filter.variable_box_gather_strict
- filter.variable_box_gather_accelerated_apple_silicon
- filter.variable_box_gather_accelerated_x86_64
numeric_reference: E
oracle_scope: mathematical_reference
research_sources:
- S01
---

# FIL-18A: variable_box_gather

Variable-radius square-box mean. Status **Proposed / D1_draft**.

Inherits the [FIL-18 family contract](FIL-18_contract.md), [FILTER common contract](FILTER_common_contract.md), [NUM numerical/precision contract](../../01-numeric/op_specs/NUM_common_contract.md), and [FMT metadata/straight-semantics contract](../../02-format-color/op_specs/FMT_common_contract.md). The family contract and this member together form the complete draft. Do not override the RN, underflow, or accelerated-error baseline.

## Ports, shape, and semantics

``input,radius` → `output,valid`; radius is a Float32/Float64 scalar `[1]` or H/W field, with explicit broadcast=scalar|spatial; batches match and radius is shared across components.

Except for semantic entry points explicitly defined by this member, inputs are raw numerical fields: channel count does not imply RGB/alpha, and attached color descriptions do not broaden sample-domain validation. Axes, positive extents, explicit broadcasting, canonical planar publication, and retention or reconstruction of applicable metadata follow the shared contract. Dependencies for components and multiple outputs are declared per request; associated collections follow the FilterBands/FrequencyGrid draft.

## Static parameters and valid domains

max_radius is finite and ≥0; axes are explicit; boundary=truncate|clamp|reflect_half|reflect_whole|wrap; radius is in [0,max_radius]; no cval.

Exceptional values and floating-point overflow follow the corresponding NUM operations and the [FILTER common contract](FILTER_common_contract.md). Do not reject NaN/Inf or turn ordinary floating-point overflow into failure under a category-wide rule. Parameter domains, structure, integer overflow, resources, cancellation, and upstream errors remain governed by their contracts. Every parameter must explicitly state its type, valid domain, and value; constructors provide no implicit defaults. Floating-point parameters are interpreted using their actually stored values.

## Mathematical reference and rounding boundaries

Numerical class: **E**; see [FILTER_numeric_reference](FILTER_numeric_reference.md) for the E/B/S notation.

At each q, read the exact stored floating-point value r. Candidate integer offsets dx/dy range over ±floor(r); retain those with max(abs(dx),abs(dy))≤r. Compute an equal-weight mean over valid taps with one exact RN_t. The center is always included; valid is always 1, but r is still validated. At r=0, explicitly bit-copy the center.

When a formula calls another member, preserve the RN stages produced at the call site as specified here or by the family contract; internal temporaries must not introduce undeclared rounding. Signed zero, dynamic-domain failures, gradual underflow, and output range follow the shared contract and NUM.

## Data / Control / Validation / Descriptor

radius is Control and retains its dirty witness. Data for actual taps after boundary mapping is Exact. Static max_radius provides only a Conservative halo; do not scan all radii to infer output shape.

Boundary coordinates are defined relative to the complete logical image; ROI/tile edges do not become new boundaries. Empty demand reads no payload; selection conditions retain Control dirty witnesses. Exact support must not be replaced by a convenient rectangular or Whole read; any enlargement requires an explicit Conservative plan. Output descriptors derive from static parameters and input descriptors, not input sample values.

## Algorithm, resources, and execution

Direct cost is O(PC*max_radius²). Integral-box optimizations require explicit admission for their actual scan. Infer disk row spans with exact comparisons; floating-point sqrt rounding must not omit taps.

In complexity notation, P=HW, C is the number of independent planes, A the number of taps, and T the number of steps. Arbitrary-precision limb bit complexity is additional and is not a measured benchmark. The shared protocol defines work/memory/stage admission, cancellation, failure state, and owner lifetime. Failures do not publish partial Values or CompleteBundles. Do not spill automatically, silently lower precision, or call retired implementations as fallback.

## Oracle, fixtures, and acceptance

r=0 bit-copies; at r=1 the disk has 5 taps and the square box 9. Near sqrt(2), the actual Float64 value determines whether the four corners are included. Truncation changes the valid count at edges. A radius above the limit fails locally and does not expand into a whole-image failure.

Reference functions evaluate small numerical examples; they are neither production kernels nor complete port-schema/preflight simulators. ExactRational rounds rational expressions directly to IEEE formats. For transcendental functions, only DirectedMPFR results whose interval endpoints round to the same value are strict goldens. If rounding cannot be determined, report Inconclusive rather than substituting an approximation.

`spatial.variable_gather(image, radius, *, disk=False, max_radius=10, boundary='truncate', dtype='float64')` → [source](../../../../oracle/ops/filter/oracles/spatial.py).

Fixture mapping and evidence scope: see [oracle coverage](../oracle-coverage.md).

See the [oracle README](../../../../oracle/ops/filter/README.md) for reproduction and evidence levels. The listed self-tests validate only the reference program; parameter boundaries, full rank/batch, metadata, ROI/dirty behavior, budgets, cancellation, and ownership still require [runtime acceptance](FILTER_oracle_protocol.md). Passing an identity/constant fixture does not accept the entire algorithm or every configuration.

## Backend and registration gates

The keys above are naming proposals only; neither strict nor either CPU-accelerated variant is registered or implemented. Accelerated variants must meet NUM final-output FP32-scaled four-ULP bounds and provide strict fallback. Do not accumulate budgets by tap, axis, stage, or iteration; thresholds, ordering, boundaries, copies, and output support requiring exact selection cannot be changed approximately.

Float64 accelerated execution still uses Float64 inputs, outputs, and exponent range; it does not first convert to Float32. Values outside the Float32-scaled range use strict NUM fallback. No kernel/CPU/ISA performance or differential acceptance was run for this package.

## Conceptual DAG (not an existing API)

`static + descriptors → preflight / demand → Data and Control declared by this member → exact expression / declared RN stages → requested owned output / complete result`

This sketch adds no implicit color conversion, hidden alpha premultiplication, automatic spectral correction, or zero-filling of missing data. Whole execution and multistage buffers follow this member and its family contract.

## Rationale and open decisions

[S01 · OpenCV Image Filtering](../research-sources.md#s01)

External sources provide algorithmic or definitional background. The finite windows, rounding, ties, units, defaults, and execution profile here are explicit project choices and do not claim bitwise identity with any library or commercial software.
