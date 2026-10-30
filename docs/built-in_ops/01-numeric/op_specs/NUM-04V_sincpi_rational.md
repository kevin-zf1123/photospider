---
spec_schema_version: 1
id: NUM-04V
parent_id: NUM-04
function: sincpi_rational
proposed_operation_keys:
  - numeric.sincpi_rational_strict
  - numeric.sincpi_rational_accelerated_apple_silicon
  - numeric.sincpi_rational_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
---

# NUM-04V: sincpi_rational

Numeric profile: strict is the exact reference defined below. Floating arithmetic in accelerated profiles follows the shared [final FP32 four-ULP contract](NUM_accelerated_contract.md). Discrete results, copies, selected endpoints and special values are exact.

Computes sin(pi*p/q)/(pi*p/q) with exact pi and the exact Int64 fraction p/q. Inherits the [rational pi contract](NUM-04_rational_pi_contract.md) in full: ordered numerator/denominator inputs, identical positive rank-1..8 shapes, q>0, unreduced fractions, floating output dtype (default Float64), and the exact-reduction rules. The output `values` preserves shape and carries no domain description. No floating division or conversion of p/q defines this operation. Keys: `numeric.sincpi_rational_strict`, `numeric.sincpi_rational_accelerated_apple_silicon`, `numeric.sincpi_rational_accelerated_x86_64`.

## Mathematics

The removable zero p=0 gives +1. Nonzero integer ratios give +0; the full quotient is even in p. Other nonzero accelerated results allow four ULP, including p/q=1/2. Strict correctly rounds the entire mathematical function directly to the output dtype. Accelerated keeps the final four-ULP bound and the exact special-case rules of the shared contract. The common-angle exact values do not strengthen this whole-quotient tolerance. No NaN payload propagation applies to the two integer ports. Positive q is an unchecked caller precondition, including for a zero numerator.

## Acceptance

Fixture: p=[0,1,-1,1], q=[1,1,1,2] gives [1,+0,+0,RN_dtype(2/pi)]. The RN references are exact for strict and for the exact common-angle cases; other nonzero accelerated outputs use the four-ULP allowance. Compare unreduced equivalent fractions and the shared large-integer parity/pole examples against exact reduction with directed function or algebraic-root rounding, not a floating quotient fed into the floating sincpi operation.
