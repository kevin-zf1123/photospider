---
spec_schema_version: 1
id: NUM-04S
parent_id: NUM-04
function: sinpi_rational
proposed_operation_keys:
  - numeric.sinpi_rational_strict
  - numeric.sinpi_rational_accelerated_apple_silicon
  - numeric.sinpi_rational_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
repository_branch: ops-specs
repository_commit: 6617c78c
---

# NUM-04S: sinpi_rational

Compute sin(pi*p/q) with exact pi and the exact Int64 fraction p/q. Inherit the
[rational pi contract](NUM-04_rational_pi_contract.md) in full: ordered numerator/
denominator inputs, identical positive rank-1..8 shapes, q>0, unreduced fraction
support, floating output dtype default Float64, exact requested-position demand,
integer range-safe reduction, errors, resources, fallback, cache and ownership.
Output values preserves shape with empty facets. No implicit floating division
or conversion of p/q defines this operation.

## Numeric behavior and acceptance

Integers use numerator-sign zero; exact halves are +/-1. Sine is bounded in [-1,1].

Strict correctly rounds the entire mathematical function directly to output dtype.
Accelerated inherits the final four-ULP bound and exact special-case rules from
the shared contract. Reduced denominators 1,2,3,4,6 use correctly rounded common-angle paths in all three profiles.
No input NaN/payload propagation applies to the two integer ports. Invalid q
fails independently of any zero numerator identity. Unsupported platforms and
runtime resource/upstream/cancellation failures retain the shared Status contract.

Conceptual public fixture: p=[0,1,1,-1,2], q=[1,6,2,1,6] -> [0,0.5,1,-0,RN_dtype(sqrt(3)/2)].
The RN references are exact for strict and the explicitly exact common-angle
paths; other nonzero accelerated outputs use the stated allowance. Compare
unreduced equivalent fractions and the shared large-integer parity/pole examples.
Use independent exact reduction and directed function/algebraic-root rounding,
not a floating quotient fed into the old sinpi helper.

Provide a real public WorkflowDocument run with numerator/denominator bindings,
static dtype and named values when implemented. Verify actual sparse read/dirty
sets, bad unrequested denominators, all valid input layouts, source changes,
cache-off, low limb/work budgets, cancellation, fallback reporting and result
lifetime. These new keys remain unimplemented; no product/platform result is
claimed. The existing floating sinpi specification retains its own input domain.
