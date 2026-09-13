---
spec_schema_version: 1
id: NUM-04P
parent_id: NUM-04
function: tanpi
proposed_operation_keys:
  - numeric.tanpi_strict
  - numeric.tanpi_accelerated_apple_silicon
  - numeric.tanpi_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
repository_branch: ops-specs
repository_commit: 30478d33
---

# NUM-04P: tanpi

Inherit the [NUM baseline](NUM_common_contract.md) for specification status,
registration, shared execution and acceptance requirements; explicit rules below
and in the named family contract take precedence.

Compute the mathematical tan(pi*x) using exact pi and the exact real value
of the input float. Inherit the [trigonometric contract](NUM-04_trigonometric_contract.md)
and [unary contract](NUM-04_unary_contract.md), including execution, resources,
NaN handling, platform availability, strict fallback and acceptance obligations.

One input `input` and one output `values` have identical shape and dtype,
Float32 or Float64. Output facets are empty. There are no static numeric
parameters, implicit casts or rounded-pi multiplication in the definition.

## Function-specific values and quality

| Input | Output |
| --- | --- |
| Integer x, including signed zero | Zero with sign(x) |
| n+1/2 | Fixed positive quiet NaN; success |
| n+1/4 / n+3/4 | Exactly +1 / -1 |
| ±Inf | Fixed positive quiet NaN; success |
| NaN | Quiet input NaN preserving payload/sign |

Strict rounds the exact mathematical function directly to output dtype.
Accelerated finite nonzero results allow at most four output-dtype ULP;
classification, zero signs and named landmarks match strict exactly. All
finite inputs are supported, including large integers and subnormal inputs.
The shared contract defines exact quarter-angle rounding and zero conventions.

Exact half-integer poles yield NaN; values merely near a pole use the actual
function. This rule must be adapted if a backend returns infinity at a pole.

## Acceptance and current status

Conceptual public fixture: `[0,0.25,0.5,0.75,1]` -> `[+0,1,canonical_NaN,-1,+0]`. Bind the array to input in a
WorkflowDocument, compile each selected key and inspect values through
ExecutionContext in both dtypes. Validate the exact bit patterns at landmarks,
huge positive/negative integers, adjacent representable values around half and
quarter integers, subnormals, NaN payloads and signed zeros. Use an independent
high-precision oracle for ordinary arguments and midpoint-sensitive cases.

Apply shared disjoint demand, typed-validation closure, invalidation, ownership,
cache, cancellation and resource checks. Verify unsupported accelerated ranges
actually use strict fallback and report it. Float32 LLVM libc is a candidate
as documented in the shared contract; it supplies no implicit Float64 coverage.
These keys are not registered. Public runnable tests and platform measurements
remain implementation deliverables; this document claims no execution results.
