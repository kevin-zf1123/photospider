---
spec_schema_version: 1
id: NUM-04U
parent_id: NUM-04
function: tanpi_rational
proposed_operation_keys:
  - numeric.tanpi_rational_strict
  - numeric.tanpi_rational_accelerated_apple_silicon
  - numeric.tanpi_rational_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
repository_branch: ops-specs
repository_commit: 6617c78c
---

# NUM-04U: tanpi_rational

Compute tan(pi*p/q) with exact pi and the exact Int64 fraction p/q. Inherit the
[rational pi contract](NUM-04_rational_pi_contract.md) in full: ordered numerator/
denominator inputs, identical positive rank-1..8 shapes, q>0, unreduced fraction
support, floating output dtype default Float64, exact requested-position demand,
integer range-safe reduction, errors, resources, fallback, cache and ownership.
Output values preserves shape with empty facets. No implicit floating division
or conversion of p/q defines this operation.

## Numeric behavior and acceptance

Integer zeros use numerator sign; exact half-integer poles return canonical NaN successfully. Quarter angles are exactly +/-1.

Strict correctly rounds the entire mathematical function directly to output dtype.
Accelerated inherits the final four-ULP bound and exact special-case rules from
the shared contract. Reduced denominators 1,2,3,4,6 use correctly rounded common-angle paths in all three profiles.
No input NaN/payload propagation applies to the two integer ports. Invalid q
fails independently of any zero numerator identity. Unsupported platforms and
runtime resource/upstream/cancellation failures retain the shared Status contract.

Conceptual public fixture: p=[0,1,1,3,1], q=[1,4,2,4,3] -> [+0,1,canonical_NaN,-1,RN_dtype(sqrt(3))].
The RN references are exact for strict and the explicitly exact common-angle
paths; other nonzero accelerated outputs use the stated allowance. Compare
unreduced equivalent fractions and the shared large-integer parity/pole examples.
Use independent exact reduction and directed function/algebraic-root rounding,
not a floating quotient fed into the old tanpi helper.

Provide a real public WorkflowDocument run with numerator/denominator bindings,
static dtype and named values through the maintained public workflow. Verify actual
sparse read/dirty sets, invalid denominators, source changes, cache-off, budgets,
cancellation, fallback reporting and result lifetime. The validation boundary is
recorded below; local timing is recorded in the implementation notes. The existing floating tanpi specification retains its own input domain.

## Maintained implementation and validation

This operation is registered in `plugins/ops/01-numeric/numeric_unary.cpp` and
exposed through `photospider/numeric/unary.hpp`. It uses exact pointwise Data,
separate typed validation and Atom-scoped failures. Its numerical path follows
[the shared implementation notes](../math-implementation.md).

The [public workflow and commands](../../../../examples/numeric_workflow/README.md)
cover this operation. The combined NUM-04 family suite passed 7,524 independent
integer/Fraction/MPFR cases per profile: Clang 21 strict/Apple locally (MPFR 4.2.2)
and Clang 18 strict/AVX2 in Ubuntu WSL (MPFR 4.2.1). Expanded manual checks and
local installed consumers passed. [Validation and native timing](../math-implementation.md#num-04-validation-and-native-timing)
record the scope and limitations. Manual targets have no CTest/integration
registration; MPFR is used only by the independent Python oracle.
