---
spec_schema_version: 1
id: FIL-13B
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
parent_id: FIL-13
function: hysteresis_edges
proposed_operation_keys:
- filter.hysteresis_edges_strict
numeric_reference: discrete exact
oracle_scope: mathematical_reference
research_sources:
- S06
---

# FIL-13B: hysteresis_edges

Weak/strong edge connectivity. Status **Proposed / D1_draft**.

Inherits the [FIL-13 family contract](FIL-13_contract.md), [FILTER common contract](FILTER_common_contract.md), [NUM numerical/precision contract](../../01-numeric/op_specs/NUM_common_contract.md), and [FMT metadata/straight-semantics contract](../../02-format-color/op_specs/FMT_common_contract.md). The family contract and this member together form the complete draft. Do not override the RN, underflow, or accelerated-error baseline.

## Ports, shape, and semantics

`weak,strong` are same-shaped UInt8 inputs with values 0/1 → `edges` UInt8.

Ports carry Results with fixed Tensor member schemas. Unless the member defines an explicit color interpretation, samples are raw numerical values; channel count does not imply RGB or alpha. Color interpretation is explicit configuration. Positive extents, axes and explicit broadcasting are checked during static construction. Materialized output tiles have independent allocations. FilterBands uses explicit SDK collections and fixed Result members under the collection contract; FrequencyGrid remains a separate proposed representation; lowering records each selected output's fixed reads and writes.

## Static parameters and valid domains

Axes are explicit; connectivity=4|8; callers guarantee binary mask encoding and strong⊆weak over the full domain without a separate validation scan.

Exceptional values and floating-point overflow follow the corresponding NUM operations and the [FILTER common contract](FILTER_common_contract.md). Do not reject NaN/Inf or turn ordinary floating-point overflow into failure under a category-wide rule. Static structural, parameter-type and capacity-arithmetic errors retain pre-execution diagnostics. Numerical-domain premises are unchecked; resource, cancellation and upstream failures retain their contracts. Every parameter must explicitly state its type, valid domain, and value; constructors provide no implicit defaults. Floating-point parameters are interpreted using their actually stored values.

## Mathematical reference and rounding boundaries

Numerical class: **discrete exact**；E/B/S notation is defined in [FILTER_numeric_reference](FILTER_numeric_reference.md).

Retain pixels in the weak graph that share a connected component with at least one strong pixel; set all others to zero. Adjacency is restricted to the original image: no wrap/reflect and no traversal through mask value 0.

When a formula calls another member, preserve the RN stages produced at the call site as specified here or by the family contract; internal temporaries must not introduce undeclared rounding. Signed zero, numerical-domain premises and explicit valid-input no-solution outcomes, gradual underflow, and output range follow the shared contract and NUM.

## Fixed reads, algorithmic control and schemas

complete-input for each plane; both weak and strong inputs are full-image Control/Data.

Boundary coordinates use the complete logical image; ROI and tile edges are not new image boundaries. Lowering closes mathematical sample support on fixed input member-tile grids. Data-dependent selection uses a finite static superset of every address the task may consume; runtime selection does not expand the Plan. Data and control are algorithmic inputs, with no separate Validation read set. Empty retained selections introduce no payload tasks. Output schemas derive only from static parameters and input schemas. Numerical, encoding and association premises remain unchecked under the common contract.

## Algorithm, resources, and execution

O(P). BFS/union-find results are unique and traversal order does not affect output. Queue/label budgets are explicit.

In complexity notation, P=HW, C is the number of independent planes, A the number of taps, and T the number of steps. Arbitrary-precision limb bit complexity is additional and is not a measured benchmark. The shared protocol defines work/memory/stage admission, cancellation, failure state, and owner lifetime. A failed call publishes no successful Result export; previously retired call exports remain valid, and final RET results require successful Run completion. Do not spill automatically, silently lower precision, or call retired implementations as fallback.

## Oracle, fixtures, and acceptance

weak=[1,1,1,0,1], strong=[1,0,0,0,0] → [1,1,1,0,0]; diagonal connectivity differs between 4 and 8; strong⊄weak violates the caller premise; any diagnostic belongs only to oracle input checking and is not a production failure requirement.

Reference functions evaluate small numerical examples; they are neither production kernels nor complete port-schema/preflight simulators. ExactRational rounds rational expressions directly to IEEE formats. For transcendental functions, only DirectedMPFR results whose interval endpoints round to the same value are strict goldens. If rounding cannot be determined, report Inconclusive rather than substituting an approximation.

`fil13.spatial.hysteresis(weak, strong, connectivity=8)` → [source](../../../../oracle/ops/filter/fil13/spatial.py).

Fixture mapping and evidence scope: see [oracle coverage](../oracle-coverage.md).

See the [oracle README](../../../../oracle/ops/filter/README.md) for reproduction and evidence levels. The listed self-tests validate only the reference program; parameter boundaries, full rank/batch, metadata, compiled ROI/tile behavior, budgets, cancellation, and ownership still require [runtime acceptance](FILTER_oracle_protocol.md). Passing an identity/constant fixture does not accept the entire algorithm or every configuration.

## Backend and registration

The key listed above is registered as a CPU/Host operation through fixed invocation ABI 8.

The registered key is `filter.hysteresis_edges_strict`. `edges.cpp` lowers each selected set of output tiles to the complete required nonspatial plane, performs 4/8-connected propagation in admitted scratch, and writes only the selected output tiles. A 32-bit queue is used where the plane size permits, with a 64-bit path for larger admitted planes. Queue, visited state and read caches are invocation-local.

Runtime verification uses the public compiler, prepared execution and Result-read interfaces and covers finite oracle comparisons, selected/empty output requests and relevant exceptional/resource/ownership cases. Specification acceptance and draft maturity are unchanged; measured runtime evidence is distinct from oracle self-checks.

## Execution model

`static parameters + schemas → compile fixed reads/tasks → prepare → unchecked arithmetic with declared RN stages → complete tiles → ordered call retirement`

This sketch preserves explicit color interpretation, alpha arithmetic, spectral formulas and missing-coverage semantics. Computation scope and multistage storage follow this member and its family contract; shared work is represented by explicit tasks and private SSA outputs.

## Rationale and open decisions

[S06 · scikit-image feature](../research-sources.md#s06)

External sources provide algorithmic or definitional background. The finite windows, rounding, ties, units, defaults, and execution profile here are explicit project choices and do not claim bitwise identity with any library or commercial software.
