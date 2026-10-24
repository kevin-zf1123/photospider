---
spec_schema_version: 1
id: FIL-18B
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
parent_id: FIL-18
function: variable_disk_gather
proposed_operation_keys:
- filter.variable_disk_gather_strict
- filter.variable_disk_gather_accelerated_apple_silicon
- filter.variable_disk_gather_accelerated_x86_64
numeric_reference: E
oracle_scope: mathematical_reference
research_sources:
- S01
---

# FIL-18B: variable_disk_gather

Variable-radius disk mean. Status **Proposed / D1_draft**.

Inherits the [FIL-18 family contract](FIL-18_contract.md), [FILTER common contract](FILTER_common_contract.md), [NUM numerical/precision contract](../../01-numeric/op_specs/NUM_common_contract.md), and [FMT metadata/straight-semantics contract](../../02-format-color/op_specs/FMT_common_contract.md). The family contract and this member together form the complete draft. Do not override the RN, underflow, or accelerated-error baseline.

## Ports, shape, and semantics

`input,radius` → `output,valid`; radius is a Float32/Float64 scalar `[1]` or H/W field, with explicit broadcast=scalar|spatial; batches match and radius is shared across components.

Ports carry Results with fixed Tensor member schemas. Unless the member defines an explicit color interpretation, samples are raw numerical values; channel count does not imply RGB or alpha. Color interpretation is explicit configuration. Positive extents, axes and explicit broadcasting are checked during static construction. Materialized output tiles have independent allocations. FilterBands and FrequencyGrid are proposed SDK/workflow representations under the collection contract; lowering records each selected output's fixed reads and writes.

## Static parameters and valid domains

Required Int64 `y_axis` and `x_axis` address distinct spatial axes. Required Int64 `component_axis` is -1 for no component axis, or a distinct nonspatial axis. Required String `broadcast` is `scalar` or `spatial`; scalar radius has sample shape [1], while spatial radius matches the complete input sample shape with component extent 1. Radius dtype is independently Float32 or Float64. Required Float64 `max_radius` is finite and nonnegative; its floor and doubled support must fit static coordinate/capacity arithmetic. Required String `boundary` is `truncate|clamp|reflect_half|reflect_whole|wrap`. Radius payload lies in [0,max_radius] as an unchecked caller premise. There is no cval parameter.

Exceptional values and floating-point overflow follow the corresponding NUM operations and the [FILTER common contract](FILTER_common_contract.md). Do not reject NaN/Inf or turn ordinary floating-point overflow into failure under a category-wide rule. Static structural, parameter-type and capacity-arithmetic errors retain pre-execution diagnostics. Numerical-domain premises are unchecked; resource, cancellation and upstream failures retain their contracts. Every parameter must explicitly state its type, valid domain, and value; constructors provide no implicit defaults. Floating-point parameters are interpreted using their actually stored values.

## Mathematical reference and rounding boundaries

Numerical class: **E**; see [FILTER_numeric_reference](FILTER_numeric_reference.md) for the E/B/S notation.

At each q, read the exact stored floating-point value r. Candidate integer offsets dx/dy range over ±floor(r); retain those satisfying dx²+dy²≤r² using an exact comparison. Compute an equal-weight mean over valid taps with one exact RN_t. The center is always included; valid is always 1 under the caller premise 0≤r≤max_radius; r is not validated. At r=0, explicitly bit-copy the center.

When a formula calls another member, preserve the RN stages produced at the call site as specified here or by the family contract; internal temporaries must not introduce undeclared rounding. Signed zero, numerical-domain premises and explicit valid-input no-solution outcomes, gradual underflow, and output range follow the shared contract and NUM.

## Fixed reads, algorithmic control and schemas

`valid` has the input sample shape and UInt8 dtype. A valid-only request produces ones without reading either payload. At static `max_radius=0`, the numerical output reads only source and copies its bits.

Radius is algorithmic control. Lowering declares a fixed source-tile superset from static max_radius and boundary mapping. At runtime the exact radius selects participating terms within the admitted views. It does not add producers, change output shape or introduce a radius-domain validation scan.

Boundary coordinates use the complete logical image; ROI and tile edges are not new image boundaries. Lowering closes mathematical sample support on fixed input member-tile grids. Data-dependent selection uses a finite static superset of every address the task may consume; runtime selection does not expand the Plan. Data and control are algorithmic inputs, with no separate Validation read set. Empty retained selections introduce no payload tasks. Output schemas derive only from static parameters and input schemas. Numerical, encoding and association premises remain unchecked under the common contract.

## Algorithm, resources, and execution

Direct cost is O(PC*max_radius²). For admitted radii at most 16, a per-task mapped halo and certified exact row prefixes accelerate the same footprint; uncertain sums, signed-zero cases and exceptional values return to ordered tap arithmetic. Larger radii use the unrestricted direct path. Integral-box optimizations require explicit admission for their actual scan. Infer disk row spans with exact comparisons; floating-point sqrt rounding must not omit taps.

In complexity notation, P=HW, C is the number of independent planes, A the number of taps, and T the number of steps. Arbitrary-precision limb bit complexity is additional and is not a measured benchmark. The shared protocol defines work/memory/stage admission, cancellation, failure state, and owner lifetime. A failed call publishes no successful Result export; previously retired call exports remain valid, and final RET results require successful Run completion. Do not spill automatically, silently lower precision, or call retired implementations as fallback.

## Oracle, fixtures, and acceptance

r=0 bit-copies; at r=1 the disk has 5 taps and the square box 9. Near sqrt(2), the actual Float64 value determines whether the four corners are included. Truncation changes the valid count at edges. A radius above max_radius violates the caller premise; its reference-tool diagnostic is not a runtime failure requirement.

Reference functions evaluate small numerical examples; they are neither production kernels nor complete port-schema/preflight simulators. ExactRational rounds rational expressions directly to IEEE formats. For transcendental functions, only DirectedMPFR results whose interval endpoints round to the same value are strict goldens. If rounding cannot be determined, report Inconclusive rather than substituting an approximation.

`fil18.spatial.variable_gather(image, radius, *, disk=False, max_radius=10, boundary='truncate', dtype='float64')` → [source](../../../../oracle/ops/filter/fil18/spatial.py).

Fixture mapping and evidence scope: see [oracle coverage](../oracle-coverage.md).

See the [oracle README](../../../../oracle/ops/filter/README.md) for reproduction and evidence levels. The listed self-tests validate only the reference program; parameter boundaries, full rank/batch, metadata, compiled ROI/tile behavior, budgets, cancellation, and ownership still require [runtime acceptance](FILTER_oracle_protocol.md). Passing an identity/constant fixture does not accept the entire algorithm or every configuration.

## Backend and registration gates

All three keys above are registered CPU operations using fixed invocation ABI 8. Strict and Apple Silicon execution have been exercised on Apple M5; x86_64 registration and unavailable-backend rejection were checked, but execution on x86_64 hardware was not performed. Accelerated variants must meet NUM final-output FP32-scaled four-ULP bounds and provide strict fallback. Do not accumulate budgets by tap, axis, stage, or iteration; thresholds, ordering, boundaries, copies, and output support requiring exact selection cannot be changed approximately.

Float64 accelerated execution still uses Float64 inputs, outputs, and exponent range; it does not first convert to Float32. Values outside the Float32-scaled range use strict NUM fallback. The implementation uses certified fast paths with strict rational/directed-interval fallback. Independent reference comparisons include both dtypes on small arrays, Float32 32×32/128×128/1024²/4096² image workloads, and targeted compiled ROI, batch, bypass, resource and cancellation cases. These checks do not establish acceptance of every possible input.

## Conceptual DAG (not an existing API)

`static parameters + schemas → compile fixed reads/tasks → prepare → unchecked arithmetic with declared RN stages → complete tiles → ordered call retirement`

This sketch preserves explicit color interpretation, alpha arithmetic, spectral formulas and missing-coverage semantics. Computation scope and multistage storage follow this member and its family contract; shared work is represented by explicit tasks and private SSA outputs.

## Rationale and open decisions

[S01 · OpenCV Image Filtering](../research-sources.md#s01)

External sources provide algorithmic or definitional background. The finite windows, rounding, ties, units, defaults, and execution profile here are explicit project choices and do not claim bitwise identity with any library or commercial software.
