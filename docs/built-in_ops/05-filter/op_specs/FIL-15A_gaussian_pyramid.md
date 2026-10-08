---
spec_schema_version: 1
id: FIL-15A
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D2_draft
implementation_status: not_implemented
parent_id: FIL-15
function: gaussian_pyramid
proposed_operation_keys:
- filter.gaussian_pyramid_strict
- filter.gaussian_pyramid_accelerated_apple_silicon
- filter.gaussian_pyramid_accelerated_x86_64
numeric_reference: S
oracle_scope: mathematical_reference
research_sources:
- S07
---

# FIL-15A: gaussian_pyramid

Gaussian pyramid. Status **Proposed / D2_draft**.

Inherits the [FIL-15 family contract](FIL-15_contract.md), [FILTER common contract](FILTER_common_contract.md), [NUM numerical/precision contract](../../01-numeric/op_specs/NUM_common_contract.md), and [FMT metadata/straight-semantics contract](../../02-format-color/op_specs/FMT_common_contract.md). The family contract and this member together form the complete draft. Do not override the RN, underflow, or accelerated-error baseline.

## Ports, shape, and semantics

`input` → FilterBands/v1 `bands`, with roles matching the family algorithm.

Except for semantic entry points explicitly defined by this member, inputs are raw numerical fields: channel count does not imply RGB/alpha, and attached color descriptions do not broaden sample-domain validation. Axes, positive extents, explicit broadcasting, canonical planar publication, and retention or reconstruction of applicable metadata follow the shared contract. Dependencies for components and multiple outputs are declared per request; associated collections follow the FilterBands/FrequencyGrid draft.

## Static parameters and valid domains

Axes are explicit; levels L≥0. Do not add levels after repeated reduction reaches H=W=1 (L is at most the number of levels needed to reach 1×1). The profile is fixed to pyramid_binomial5_v1; copy all non-spatial axes.

Exceptional values and floating-point overflow follow the corresponding NUM operations and the [FILTER common contract](FILTER_common_contract.md). Do not reject NaN/Inf or turn ordinary floating-point overflow into failure under a category-wide rule. Parameter domains, structure, integer overflow, resources, cancellation, and upstream errors remain governed by their contracts. Every parameter must explicitly state its type, valid domain, and value; constructors provide no implicit defaults. Floating-point parameters are interpreted using their actually stored values.

## Mathematical reference and rounding boundaries

Numerical class: **S**; see [FILTER_numeric_reference](FILTER_numeric_reference.md) for the E/B/S notation.

G0 is the read-only owner of input; G(l+1)=reduce(G_l). Publish G0..GL, with each level an explicit RN_t stage.

When a formula calls another member, preserve the RN stages produced at the call site as specified here or by the family contract; internal temporaries must not introduce undeclared rounding. Signed zero, dynamic-domain failures, gradual underflow, and output range follow the shared contract and NUM.

## Data / Control / Validation / Descriptor

Publish the complete association descriptors, roles, dtype/shape/phase, but execute payloads lazily by band. A requested band reads only that band and its mathematical ancestors; a deeper low-frequency request may require preceding low-frequency levels, but never eagerly materializes sibling bands. At L=0, identity retains only the source/base owner.

Boundary coordinates are defined relative to the complete logical image; ROI/tile edges do not become new boundaries. Empty demand reads no payload; selection conditions retain Control dirty witnesses. Exact support must not be replaced by a convenient rectangular or Whole read; any enlargement requires an explicit Conservative plan. Output descriptors derive from static parameters and input descriptors, not input sample values.

## Algorithm, resources, and execution

The geometric-series pixel total is O(PC), with 25 taps per pixel. Budget active ancestors and required levels; do not automatically release bands still owned by a Result.

In complexity notation, P=HW, C is the number of independent planes, A the number of taps, and T the number of steps. Arbitrary-precision limb bit complexity is additional and is not a measured benchmark. The shared protocol defines work/memory/stage admission, cancellation, failure state, and owner lifetime. Failures do not publish partial Values or CompleteBundles. Do not spill automatically, silently lower precision, or call retired implementations as fallback.

## Oracle, fixtures, and acceptance

Constant inputs produce identical G levels and +0 details. For odd 3×5 dimensions, preserve each level shape. L=0 identity preserves signed zero. Editing one detail changes only its corresponding reconstruction scale. Numerical-error checks must not require arbitrary floats to round-trip losslessly.

Reference functions evaluate small numerical examples; they are neither production kernels nor complete port-schema/preflight simulators. ExactRational rounds rational expressions directly to IEEE formats. For transcendental functions, only DirectedMPFR results whose interval endpoints round to the same value are strict goldens. If rounding cannot be determined, report Inconclusive rather than substituting an approximation.

`multiscale.pyramid(image=None, levels=0, *, mode='laplacian', bands=None, dtype='float64')` → [source](../../../../oracle/ops/filter/oracles/multiscale.py).

Fixture mapping and evidence scope: see [oracle coverage](../oracle-coverage.md).

See the [oracle README](../../../../oracle/ops/filter/README.md) for reproduction and evidence levels. The listed self-tests validate only the reference program; parameter boundaries, full rank/batch, metadata, ROI/dirty behavior, budgets, cancellation, and ownership still require [runtime acceptance](FILTER_oracle_protocol.md). Passing an identity/constant fixture does not accept the entire algorithm or every configuration.

## Backend and registration gates

The keys above are naming proposals only; neither strict nor either CPU-accelerated variant is registered or implemented. Accelerated variants must meet NUM final-output FP32-scaled four-ULP bounds and provide strict fallback. Do not accumulate budgets by tap, axis, stage, or iteration; thresholds, ordering, boundaries, copies, and output support requiring exact selection cannot be changed approximately.

Float64 accelerated execution still uses Float64 inputs, outputs, and exponent range; it does not first convert to Float32. Values outside the Float32-scaled range use strict NUM fallback. No kernel/CPU/ISA performance or differential acceptance was run for this package.

## Conceptual DAG (not an existing API)

`static + descriptors → preflight / demand → Data and Control declared by this member → exact expression / declared RN stages → requested owned output / complete result`

This sketch adds no implicit color conversion, hidden alpha premultiplication, automatic spectral correction, or zero-filling of missing data. Whole execution and multistage buffers follow this member and its family contract.

## Rationale and open decisions

[S07 · Paris, Hasinoff, Kautz: Local Laplacian Filters](../research-sources.md#s07)

External sources provide algorithmic or definitional background. The finite windows, rounding, ties, units, defaults, and execution profile here are explicit project choices and do not claim bitwise identity with any library or commercial software.
