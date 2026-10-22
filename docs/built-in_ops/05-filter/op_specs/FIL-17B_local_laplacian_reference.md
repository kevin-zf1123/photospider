---
spec_schema_version: 1
id: FIL-17B
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D2_draft
implementation_status: not_implemented
parent_id: FIL-17
function: local_laplacian_reference
proposed_operation_keys:
- filter.local_laplacian_reference_strict
- filter.local_laplacian_reference_accelerated_apple_silicon
- filter.local_laplacian_reference_accelerated_x86_64
numeric_reference: S+B
oracle_scope: mathematical_reference
research_sources:
- S07
---

# FIL-17B: local_laplacian_reference

Per-coefficient Local Laplacian reference. Status **Proposed / D2_draft**.

Inherits the [FIL-17 family contract](FIL-17_contract.md), [FILTER common contract](FILTER_common_contract.md), [NUM numerical/precision contract](../../01-numeric/op_specs/NUM_common_contract.md), and [FMT metadata/straight-semantics contract](../../02-format-color/op_specs/FMT_common_contract.md). The family contract and this member together form the complete draft. Do not override the RN, underflow, or accelerated-error baseline.

## Ports, shape, and semantics

`input` is single-plane → `output` with the original shape.

Ports carry Results with fixed Tensor member schemas. Unless the member defines an explicit color interpretation, samples are raw numerical values; channel count does not imply RGB or alpha. Color interpretation is explicit configuration. Positive extents, axes and explicit broadcasting are checked during static construction. Materialized output tiles have independent allocations. FilterBands and FrequencyGrid are proposed SDK/workflow representations under the collection contract; lowering records each selected output's fixed reads and writes.

## Static parameters and valid domains

Use the FIL-15 profile and levels; edge_threshold σ>0; detail_exponent α>0; edge_slope β≥0; radius is determined by the pyramid profile; no color mode.

Exceptional values and floating-point overflow follow the corresponding NUM operations and the [FILTER common contract](FILTER_common_contract.md). Do not reject NaN/Inf or turn ordinary floating-point overflow into failure under a category-wide rule. Static structural, parameter-type and capacity-arithmetic errors retain pre-execution diagnostics. Numerical-domain premises are unchecked; resource, cancellation and upstream failures retain their contracts. Every parameter must explicitly state its type, valid domain, and value; constructors provide no implicit defaults. Floating-point parameters are interpreted using their actually stored values.

## Mathematical reference and rounding boundaries

Numerical class: **S+B**; see [FILTER_numeric_reference](FILTER_numeric_reference.md) for the E/B/S notation.

First build the Gaussian pyramid G from input. At each level l<L and location q, let g=G_l[q]. For every full-resolution value v, let d=v-g and remap `r_g(v)=g+sign(d)*f(abs(d))`, where f(z)=σ*(z/σ)^α for z≤σ and σ+β*(z-σ) otherwise; sign(0)=0. Round each remapped value to RN_t, build its FIL-15B pyramid, and extract the detail at l,q as target L_lq. Reuse the original GL as the coarsest base. Reconstruct with FIL-15C. For α=β=1, explicitly bit-copy as identity; do not claim losslessness after a round trip.

When a formula calls another member, preserve the RN stages produced at the call site as specified here or by the family contract; internal temporaries must not introduce undeclared rounding. Signed zero, numerical-domain premises and explicit valid-input no-solution outcomes, gradual underflow, and output range follow the shared contract and NUM.

## Fixed reads, algorithmic control and schemas

Except for identity, execute complete-input per plane. Each reference g comes from the original G; remapping covers the full source image. A fixed halo cannot replace this whole-image reconstruction definition.

Boundary coordinates use the complete logical image; ROI and tile edges are not new image boundaries. Lowering closes mathematical sample support on fixed input member-tile grids. Data-dependent selection uses a finite static superset of every address the task may consume; runtime selection does not expand the Plan. Data and control are algorithmic inputs, with no separate Validation read set. Empty retained selections introduce no payload tasks. Output schemas derive only from static parameters and input schemas. Numerical, encoding and association premises remain unchecked under the common contract.

## Algorithm, resources, and execution

The strict per-coefficient reference has O(P² C) work because it reconstructs a pyramid for each coefficient. This complexity is not an oracle-only runtime cap: admit execution against root work, memory, stage, and retained-owner budgets. Any range-sampled fast approximation needs a separate key and does not share strict semantics.

In complexity notation, P=HW, C is the number of independent planes, A the number of taps, and T the number of steps. Arbitrary-precision limb bit complexity is additional and is not a measured benchmark. The shared protocol defines work/memory/stage admission, cancellation, failure state, and owner lifetime. A failed call publishes no successful Result export; previously retired call exports remain valid, and final RET results require successful Run completion. Do not spill automatically, silently lower precision, or call retired implementations as fallback.

## Oracle, fixtures, and acceptance

α=β=1 is identity; constants are preserved; β=0 saturates large edges; compare small images against the per-coefficient oracle; ordinary band gain produces a different result.

Reference functions evaluate small numerical examples; they are neither production kernels nor complete port-schema/preflight simulators. ExactRational rounds rational expressions directly to IEEE formats. For transcendental functions, only DirectedMPFR results whose interval endpoints round to the same value are strict goldens. If rounding cannot be determined, report Inconclusive rather than substituting an approximation.

`fil17.multiscale.local_laplacian(image, *, levels=1, edge_threshold=1, detail_exponent=1, edge_slope=1, dtype='float64')` → [source](../../../../oracle/ops/filter/fil17/multiscale.py).

Fixture mapping and evidence scope: see [oracle coverage](../oracle-coverage.md).

See the [oracle README](../../../../oracle/ops/filter/README.md) for reproduction and evidence levels. The listed self-tests validate only the reference program; parameter boundaries, full rank/batch, metadata, compiled ROI/tile behavior, budgets, cancellation, and ownership still require [runtime acceptance](FILTER_oracle_protocol.md). Passing an identity/constant fixture does not accept the entire algorithm or every configuration.

## Backend and registration gates

The keys above are naming proposals only; neither strict nor either CPU-accelerated variant is registered or implemented. Accelerated variants must meet NUM final-output FP32-scaled four-ULP bounds and provide strict fallback. Do not accumulate budgets by tap, axis, stage, or iteration; thresholds, ordering, boundaries, copies, and output support requiring exact selection cannot be changed approximately.

Float64 accelerated execution still uses Float64 inputs, outputs, and exponent range; it does not first convert to Float32. Values outside the Float32-scaled range use strict NUM fallback. No kernel/CPU/ISA performance or differential acceptance was run for this package.

## Conceptual DAG (not an existing API)

`static parameters + schemas → compile fixed reads/tasks → prepare → unchecked arithmetic with declared RN stages → complete tiles → ordered call retirement`

This sketch preserves explicit color interpretation, alpha arithmetic, spectral formulas and missing-coverage semantics. Computation scope and multistage storage follow this member and its family contract; shared work is represented by explicit tasks and private SSA outputs.

## Rationale and open decisions

[S07 · Paris, Hasinoff, Kautz: Local Laplacian Filters](../research-sources.md#s07)

External sources provide algorithmic or definitional background. The finite windows, rounding, ties, units, defaults, and execution profile here are explicit project choices and do not claim bitwise identity with any library or commercial software.
