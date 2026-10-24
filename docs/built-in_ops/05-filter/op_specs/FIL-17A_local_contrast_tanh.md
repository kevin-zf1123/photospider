---
spec_schema_version: 1
id: FIL-17A
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D2_draft
implementation_status: implemented
parent_id: FIL-17
function: local_contrast_tanh
proposed_operation_keys:
- filter.local_contrast_tanh_strict
- filter.local_contrast_tanh_accelerated_apple_silicon
- filter.local_contrast_tanh_accelerated_x86_64
numeric_reference: E
oracle_scope: mathematical_reference
research_sources:
- S07
---

# FIL-17A: local_contrast_tanh

Local contrast with bounded detail. Status **Proposed / D2_draft**.

Inherits the [FIL-17 family contract](FIL-17_contract.md), [FILTER common contract](FILTER_common_contract.md), [NUM numerical/precision contract](../../01-numeric/op_specs/NUM_common_contract.md), and [FMT metadata/straight-semantics contract](../../02-format-color/op_specs/FMT_common_contract.md). The family contract and this member together form the complete draft. Do not override the RN, underflow, or accelerated-error baseline.

## Ports, shape, and semantics

`input,base` have the same dtype and complete sample shape → `output`; the caller explicitly creates base using FIL-04B or FIL-07.

Ports carry Results with fixed Tensor member schemas. Unless the member defines an explicit color interpretation, samples are raw numerical values; channel count does not imply RGB or alpha. Color interpretation is explicit configuration. Positive extents, axes and explicit broadcasting are checked during static construction. Materialized output tiles have independent allocations. FilterBands and FrequencyGrid are proposed SDK/workflow representations under the collection contract; lowering records each selected output's fixed reads and writes.

## Static parameters and valid domains

Both parameters are required Float64 values: `amount` is finite; `detail_scale` is finite and strictly positive. There are no implicit defaults or intensity-range assumptions.

Exceptional values and floating-point overflow follow the corresponding NUM operations and the [FILTER common contract](FILTER_common_contract.md). Do not reject NaN/Inf or turn ordinary floating-point overflow into failure under a category-wide rule. Static structural, parameter-type and capacity-arithmetic errors retain pre-execution diagnostics. Numerical-domain premises are unchecked; resource, cancellation and upstream failures retain their contracts. Every parameter must explicitly state its type, valid domain, and value; constructors provide no implicit defaults. Floating-point parameters are interpreted using their actually stored values.

## Mathematical reference and rounding boundaries

Numerical class: **E**; see [FILTER_numeric_reference](FILTER_numeric_reference.md) for the E/B/S notation.

At amount=0, bit-copy input; otherwise D=input-base exactly and `output=RN_t(input+amount*s*tanh(D/s))`. s has the units of input. This is not a vendor Clarity reproduction.

When a formula calls another member, preserve the RN stages produced at the call site as specified here or by the family contract; internal temporaries must not introduce undeclared rounding. Signed zero, numerical-domain premises and explicit valid-input no-solution outcomes, gradual underflow, and output range follow the shared contract and NUM.

## Fixed reads, algorithmic control and schemas

Pointwise in input and base. At `amount=0`, only input payload is read; base retains its static schema obligation. The base node itself may have a halo/complete-input dependency, but this member does not redefine its algorithm.

Boundary coordinates use the complete logical image; ROI and tile edges are not new image boundaries. Lowering closes mathematical sample support on fixed input member-tile grids. Data-dependent selection uses a finite static superset of every address the task may consume; runtime selection does not expand the Plan. Data and control are algorithmic inputs, with no separate Validation read set. Empty retained selections introduce no payload tasks. Output schemas derive only from static parameters and input schemas. Numerical, encoding and association premises remain unchecked under the common contract.

## Algorithm, resources, and execution

O(PC) with certified tanh evaluation. RN32 endpoint certificates accelerate ordinary Float32 inputs. Exact rational bounds retain the sign of tiny cancellation residuals; stable exponential expressions and a proved same-sided rounding probe handle saturation. Uncertain cases retain directed refinement. As s approaches zero, do not automatically switch to hard clipping.

In complexity notation, P=HW, C is the number of independent planes, A the number of taps, and T the number of steps. Arbitrary-precision limb bit complexity is additional and is not a measured benchmark. The shared protocol defines work/memory/stage admission, cancellation, failure state, and owner lifetime. A failed call publishes no successful Result export; previously retired call exports remain valid, and final RET results require successful Run completion. Do not spill automatically, silently lower precision, or call retired implementations as fallback.

## Oracle, fixtures, and acceptance

When input=base, finite values remain unchanged; amount=0 bypasses base; test odd response to detail; HDR values are not clipped.

Reference functions evaluate small numerical examples; they are neither production kernels nor complete port-schema/preflight simulators. ExactRational rounds rational expressions directly to IEEE formats. For transcendental functions, only DirectedMPFR results whose interval endpoints round to the same value are strict goldens. If rounding cannot be determined, report Inconclusive rather than substituting an approximation.

`fil17.transcend.local_contrast(image, base, *, amount=0, detail_scale=1, dtype='float64')` → [source](../../../../oracle/ops/filter/fil17/transcend.py).

Fixture mapping and evidence scope: see [oracle coverage](../oracle-coverage.md).

See the [oracle README](../../../../oracle/ops/filter/README.md) for reproduction and evidence levels. The listed self-tests validate only the reference program; parameter boundaries, full rank/batch, metadata, compiled ROI/tile behavior, budgets, cancellation, and ownership still require [runtime acceptance](FILTER_oracle_protocol.md). Passing an identity/constant fixture does not accept the entire algorithm or every configuration.

## Backend and registration gates

All three keys above are registered CPU operations using fixed invocation ABI 8. Strict and Apple Silicon execution have been exercised on Apple M5; x86_64 registration and unavailable-backend rejection were checked, but execution on x86_64 hardware was not performed. Accelerated variants must meet NUM final-output FP32-scaled four-ULP bounds and provide strict fallback. Do not accumulate budgets by tap, axis, stage, or iteration; thresholds, ordering, boundaries, copies, and output support requiring exact selection cannot be changed approximately.

Float64 accelerated execution still uses Float64 inputs, outputs, and exponent range; it does not first convert to Float32. Values outside the Float32-scaled range use strict NUM fallback. The implementation uses certified fast paths with strict rational/directed-interval fallback. Independent reference comparisons include both dtypes on small arrays, Float32 32×32/128×128/1024²/4096² image workloads, and targeted compiled ROI, batch, bypass, resource and cancellation cases. These checks do not establish acceptance of every possible input.

## Conceptual DAG (not an existing API)

`static parameters + schemas → compile fixed reads/tasks → prepare → unchecked arithmetic with declared RN stages → complete tiles → ordered call retirement`

This sketch preserves explicit color interpretation, alpha arithmetic, spectral formulas and missing-coverage semantics. Computation scope and multistage storage follow this member and its family contract; shared work is represented by explicit tasks and private SSA outputs.

## Rationale and open decisions

[S07 · Paris, Hasinoff, Kautz: Local Laplacian Filters](../research-sources.md#s07)

External sources provide algorithmic or definitional background. The finite windows, rounding, ties, units, defaults, and execution profile here are explicit project choices and do not claim bitwise identity with any library or commercial software.
