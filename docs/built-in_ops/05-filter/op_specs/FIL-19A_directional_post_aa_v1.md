---
spec_schema_version: 1
id: FIL-19A
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D2_draft
implementation_status: implemented
parent_id: FIL-19
function: directional_post_aa_v1
proposed_operation_keys:
- filter.directional_post_aa_v1_strict
- filter.directional_post_aa_v1_accelerated_apple_silicon
- filter.directional_post_aa_v1_accelerated_x86_64
numeric_reference: E
oracle_scope: mathematical_reference
research_sources:
- S08
---

# FIL-19A: directional_post_aa_v1

Bounded-direction post-process AA. Status **Proposed / D2_draft**.

Inherits the [FIL-19 family contract](FIL-19_contract.md), [FILTER common contract](FILTER_common_contract.md), [NUM numerical/precision contract](../../01-numeric/op_specs/NUM_common_contract.md), and [FMT metadata/straight-semantics contract](../../02-format-color/op_specs/FMT_common_contract.md). The family contract and this member together form the complete draft. Do not override the RN, underflow, or accelerated-error baseline.

## Ports, shape, and semantics

`input,edge_guide` → `output`; guide is single-plane, input components are independent, and H/W/batch dimensions match.

Ports carry Results with fixed Tensor member schemas. Unless the member defines an explicit color interpretation, samples are raw numerical values; channel count does not imply RGB or alpha. Color interpretation is explicit configuration. Positive extents, axes and explicit broadcasting are checked during static construction. Materialized output tiles have independent allocations. FilterBands and FrequencyGrid are proposed SDK/workflow representations under the collection contract; lowering records each selected output's fixed reads and writes.

## Static parameters and valid domains

Required Int64 `y_axis`, `x_axis`, and `component_axis` use the complete sample-shape axis convention. Spatial axes are distinct; component_axis is -1 or a distinct nonspatial axis. The guide matches input dtype and shape with component extent 1. Required Float64 `threshold` is finite and >0; required Float64 `blend` is finite and in [0,1/2]. These numerical premises are unchecked. Clamp is the fixed boundary; there is no boundary parameter.

Exceptional values and floating-point overflow follow the corresponding NUM operations and the [FILTER common contract](FILTER_common_contract.md). Do not reject NaN/Inf or turn ordinary floating-point overflow into failure under a category-wide rule. Static structural, parameter-type and capacity-arithmetic errors retain pre-execution diagnostics. Numerical-domain premises are unchecked; resource, cancellation and upstream failures retain their contracts. Every parameter must explicitly state its type, valid domain, and value; constructors provide no implicit defaults. Floating-point parameters are interpreted using their actually stored values.

## Mathematical reference and rounding boundaries

Numerical class: **E**; see [FILTER_numeric_reference](FILTER_numeric_reference.md) for the E/B/S notation.

Read the guide center and N/S/E/W neighbors; compute ex=abs(E-W), ey=abs(S-N) exactly. If max(ex,ey)≤threshold or blend=0, bit-copy the input center. Otherwise choose N/S when ex≥ey (along the approximate edge direction), else E/W; compute `RN_t((1-blend)*Icenter+blend*(Iu+Iv)/2)`. For a NaN control result, evaluate the written subtraction and abs nodes first, then propagate the leftmost NaN of `max(ex,ey)`; abs clears its sign. A generated NaN in ex therefore precedes an input-derived NaN in ey. Equality at the threshold does not smooth; directional ties choose N/S. This heuristic is project-defined and is not called SMAA/FXAA.

When a formula calls another member, preserve the RN stages produced at the call site as specified here or by the family contract; internal temporaries must not introduce undeclared rounding. Signed zero, numerical-domain premises and explicit valid-input no-solution outcomes, gradual underflow, and output range follow the shared contract and NUM.

## Fixed reads, algorithmic control and schemas

The guide cross is Control. An activated point reads only the selected two neighbors and center. At blend=0, no guide data is read (explicit bypass). Fixed halo is 1; selection is exact.

Boundary coordinates use the complete logical image; ROI and tile edges are not new image boundaries. Lowering closes mathematical sample support on fixed input member-tile grids. Data-dependent selection uses a finite static superset of every address the task may consume; runtime selection does not expand the Plan. Data and control are algorithmic inputs, with no separate Validation read set. Empty retained selections introduce no payload tasks. Output schemas derive only from static parameters and input schemas. Numerical, encoding and association premises remain unchecked under the common contract.

## Algorithm, resources, and execution

O(PC), with no search or temporal history; correctness does not establish acceptable visual quality.

In complexity notation, P=HW, C is the number of independent planes, A the number of taps, and T the number of steps. Arbitrary-precision limb bit complexity is additional and is not a measured benchmark. The shared protocol defines work/memory/stage admission, cancellation, failure state, and owner lifetime. A failed call publishes no successful Result export; previously retired call exports remain valid, and final RET results require successful Run completion. Do not spill automatically, silently lower precision, or call retired implementations as fallback.

## Oracle, fixtures, and acceptance

A constant guide is unchanged; a vertical step selects the along-edge direction; blend=0 preserves poisoned unread guide data; bright-line textures require human quality review.

Reference functions evaluate small numerical examples; they are neither production kernels nor complete port-schema/preflight simulators. ExactRational rounds rational expressions directly to IEEE formats. For transcendental functions, only DirectedMPFR results whose interval endpoints round to the same value are strict goldens. If rounding cannot be determined, report Inconclusive rather than substituting an approximation.

`fil19.spatial.post_aa(image, guide, *, threshold=0.1, blend=Fraction(1, 4), dtype='float64')` → [source](../../../../oracle/ops/filter/fil19/spatial.py).

Fixture mapping and evidence scope: see [oracle coverage](../oracle-coverage.md).

See the [oracle README](../../../../oracle/ops/filter/README.md) for reproduction and evidence levels. The listed self-tests validate only the reference program; parameter boundaries, full rank/batch, metadata, compiled ROI/tile behavior, budgets, cancellation, and ownership still require [runtime acceptance](FILTER_oracle_protocol.md). Passing an identity/constant fixture does not accept the entire algorithm or every configuration.

## Backend and registration gates

All three keys above are registered CPU operations using fixed invocation ABI 8. Strict and Apple Silicon execution have been exercised on Apple M5; x86_64 registration and unavailable-backend rejection were checked, but execution on x86_64 hardware was not performed. Accelerated variants must meet NUM final-output FP32-scaled four-ULP bounds and provide strict fallback. Do not accumulate budgets by tap, axis, stage, or iteration; thresholds, ordering, boundaries, copies, and output support requiring exact selection cannot be changed approximately.

Float64 accelerated execution still uses Float64 inputs, outputs, and exponent range; it does not first convert to Float32. Values outside the Float32-scaled range use strict NUM fallback. The implementation uses certified fast paths with strict rational/directed-interval fallback. Independent reference comparisons include both dtypes on small arrays, Float32 32×32/128×128/1024²/4096² image workloads, and targeted compiled ROI, batch, bypass, resource and cancellation cases. These checks do not establish acceptance of every possible input.

## Conceptual DAG (not an existing API)

`static parameters + schemas → compile fixed reads/tasks → prepare → unchecked arithmetic with declared RN stages → complete tiles → ordered call retirement`

This sketch preserves explicit color interpretation, alpha arithmetic, spectral formulas and missing-coverage semantics. Computation scope and multistage storage follow this member and its family contract; shared work is represented by explicit tasks and private SSA outputs.

## Rationale and open decisions

[S08 · Jimenez et al.: SMAA](../research-sources.md#s08)

External sources provide algorithmic or definitional background. The finite windows, rounding, ties, units, defaults, and execution profile here are explicit project choices and do not claim bitwise identity with any library or commercial software.
