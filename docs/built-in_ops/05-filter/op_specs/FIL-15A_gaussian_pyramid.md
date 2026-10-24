---
spec_schema_version: 1
id: FIL-15A
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D2_draft
implementation_status: implemented
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

Ports carry Results with fixed Tensor member schemas. Unless the member defines an explicit color interpretation, samples are raw numerical values; channel count does not imply RGB or alpha. Color interpretation is explicit configuration. Positive extents, axes and explicit broadcasting are checked during static construction. Materialized output tiles have independent allocations. FilterBands uses explicit SDK collections and fixed Result members under the collection contract; FrequencyGrid remains a separate proposed representation; lowering records each selected output's fixed reads and writes.

## Static parameters and valid domains

Axes are explicit; levels L≥0. Do not add levels after repeated reduction reaches H=W=1 (L is at most the number of levels needed to reach 1×1). The profile is fixed to pyramid_binomial5_v1; copy all non-spatial axes.

Exceptional values and floating-point overflow follow the corresponding NUM operations and the [FILTER common contract](FILTER_common_contract.md). Do not reject NaN/Inf or turn ordinary floating-point overflow into failure under a category-wide rule. Static structural, parameter-type and capacity-arithmetic errors retain pre-execution diagnostics. Numerical-domain premises are unchecked; resource, cancellation and upstream failures retain their contracts. Every parameter must explicitly state its type, valid domain, and value; constructors provide no implicit defaults. Floating-point parameters are interpreted using their actually stored values.

## Mathematical reference and rounding boundaries

Numerical class: **S**; see [FILTER_numeric_reference](FILTER_numeric_reference.md) for the E/B/S notation.

G0 is the read-only owner of input; G(l+1)=reduce(G_l). Publish G0..GL, with each level an explicit RN_t stage.

When a formula calls another member, preserve the RN stages produced at the call site as specified here or by the family contract; internal temporaries must not introduce undeclared rounding. Signed zero, numerical-domain premises and explicit valid-input no-solution outcomes, gradual underflow, and output range follow the shared contract and NUM.

## Fixed reads, algorithmic control and schemas

The complete static band directory records roles, dtype, shape and phase. Compilation selects each requested band and its mathematical ancestors; a deeper low-frequency request can require preceding low-frequency stages without materializing unrelated sibling bands. Published Results contain their complete compiled tile sets. At L=0, materialized identity tiles use explicit COPY; an authoring workflow may instead reuse an existing Result explicitly.

Boundary coordinates use the complete logical image; ROI and tile edges are not new image boundaries. Lowering closes mathematical sample support on fixed input member-tile grids. Data-dependent selection uses a finite static superset of every address the task may consume; runtime selection does not expand the Plan. Data and control are algorithmic inputs, with no separate Validation read set. Empty retained selections introduce no payload tasks. Output schemas derive only from static parameters and input schemas. Numerical, encoding and association premises remain unchecked under the common contract.

## Algorithm, resources, and execution

The geometric-series pixel total is O(PC), with 25 taps per pixel. Budget active ancestors and required levels; do not automatically release bands still owned by a Result.

In complexity notation, P=HW, C is the number of independent planes, A the number of taps, and T the number of steps. Arbitrary-precision limb bit complexity is additional and is not a measured benchmark. The shared protocol defines work/memory/stage admission, cancellation, failure state, and owner lifetime. A failed call publishes no successful Result export; previously retired call exports remain valid, and final RET results require successful Run completion. Do not spill automatically, silently lower precision, or call retired implementations as fallback.

## Oracle, fixtures, and acceptance

Constant inputs produce identical G levels and +0 details. For odd 3×5 dimensions, preserve each level shape. L=0 identity preserves signed zero. Editing one detail changes only its corresponding reconstruction scale. Numerical-error checks must not require arbitrary floats to round-trip losslessly.

Reference functions evaluate small numerical examples; they are neither production kernels nor complete port-schema/preflight simulators. ExactRational rounds rational expressions directly to IEEE formats. For transcendental functions, only DirectedMPFR results whose interval endpoints round to the same value are strict goldens. If rounding cannot be determined, report Inconclusive rather than substituting an approximation.

`fil15.multiscale.pyramid(image=None, levels=0, *, mode='laplacian', bands=None, dtype='float64')` → [source](../../../../oracle/ops/filter/fil15/multiscale.py).

Fixture mapping and evidence scope: see [oracle coverage](../oracle-coverage.md).

See the [oracle README](../../../../oracle/ops/filter/README.md) for reproduction and evidence levels. The listed self-tests validate only the reference program; parameter boundaries, full rank/batch, metadata, compiled ROI/tile behavior, budgets, cancellation, and ownership still require [runtime acceptance](FILTER_oracle_protocol.md). Passing an identity/constant fixture does not accept the entire algorithm or every configuration.

## Backend and registration

`pyramid.cpp` implements the three CPU profile keys listed above. Native members are `gaussian_0` through `gaussian_L` in schema `photospider.filter_bands.pyramid_binomial5_v1` version 1. Exact binomial coefficients and the RN_t reduce stage are preserved. Materialized level zero uses COPY; a selected band reads its required ancestors and selected nonspatial planes.

The public `PyramidOptions`, `PyramidBand`, `PyramidBands` and analysis/reconstruction helpers in `photospider/ops/filter.hpp` implement the logical FilterBands/v1 directory. Entries carry source grid, dtype, original and level shapes, level/role, crop, even-phase sampling step, origin/spacing/unit and analysis/synthesis/rounding/padding profiles. Native Results hold at most 16 members (0..15 reductions); the SDK composes blocks of at most 15 reductions for deeper legal pyramids without an additional RN stage. `filter.select_band` and `filter.assemble_laplacian_bands` are structural COPY utilities. Reconstruction checks the complete logical directory and accepts compatible reordered or edited coefficient references.

These are CPU/Host fixed-invocation-ABI-8 implementations. Strict and accelerated variants share certified arithmetic and exact fallback. Float64 inputs retain their precision and exponent range; final accelerated error does not accumulate across stages, and exact ordering, copying, exceptional values and gradual underflow are preserved. The available Apple Silicon profile was exercised against independent references. The guarded x86_64 profile is implemented but was not executed on the Apple test host.

Runtime checks use the public compiler, preparation, submission and Result-read interfaces. Numerical references, selected/empty output requests and relevant metadata, resource, cancellation and ownership paths have separate runtime evidence. Implementation does not change Proposed acceptance status or draft maturity.

## Execution model

`static parameters + schemas → compile fixed reads/tasks → prepare → unchecked arithmetic with declared RN stages → complete tiles → ordered call retirement`

This sketch preserves explicit color interpretation, alpha arithmetic, spectral formulas and missing-coverage semantics. Computation scope and multistage storage follow this member and its family contract; shared work is represented by explicit tasks and private SSA outputs.

## Rationale and open decisions

[S07 · Paris, Hasinoff, Kautz: Local Laplacian Filters](../research-sources.md#s07)

External sources provide algorithmic or definitional background. The finite windows, rounding, ties, units, defaults, and execution profile here are explicit project choices and do not claim bitwise identity with any library or commercial software.
