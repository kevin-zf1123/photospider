---
spec_schema_version: 1
id: NUM-10E
parent_id: NUM-10
function: scatter_minimum
proposed_operation_keys:
  - array.scatter_minimum_strict
  - array.scatter_minimum_accelerated_apple_silicon
  - array.scatter_minimum_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_manual_acceptance
repository_branch: ops-specs
repository_commit: 30478d33
implementation_branch: numeric-optimize
implementation_base_commit: eb0e90c8
implementation_updated: 2026-09-21
---

# NUM-10E: scatter_minimum

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

Inherit the [NUM baseline](NUM_common_contract.md) for specification status,
registration, shared execution and acceptance requirements; explicit rules below
and in the named family contract take precedence.

Inherit the [scatter contract](NUM-10_scatter_contract.md) in full: base/indices/
updates port order, static axis, same value dtype/non-axis dimensions, four
supported dtypes, generic dense output, global index validation, exact contributor
support, index invalidation, errors, resources and immutable ownership. All three
profiles produce identical bits. Input base is never modified.

## Update semantics

Select the numerical minimum including base. For floating NaNs and zero signs,
use the shared aggregate priority and NUM-05 minimum rules; this operation never
ignores a NaN or converts Int64 through floating point.

If no update matches, copy base without arithmetic, including sNaN bits. For
aggregate variants with matching updates, use the shared exceptional-value
priority and zero rules. Unrequested base/update values are not evaluated;
indices remains globally validated even for a partial output request.

## Acceptance and implementation status

Conceptual public fixture: base=[10,20,30], indices=[1,1,2], updates=[2,3,4],
axis=0 -> [10,2,4]. Use independent index grouping and the specified exact
numeric or bit-selection oracle; verify each port's actual read footprint.
Include duplicated targets, unhit sNaN base, selected and unselected failures,
NaN payload precedence, infinities, signed zeros, integer extrema, source strides,
index changes, global invalid-index rejection and shared resource/lifetime tests.

The current three profile keys use `scatter_minimum_node` from
`photospider/numeric/indexing.hpp`. Matching updates are grouped with the base
and reduced using the shared NaN-propagating minimum and signed-zero rules;
no-hit coordinates preserve base bits. On 2026-09-14, local AppleClang 21 strict/Apple and Ubuntu WSL Clang 18
strict/AVX2 passed the complete manual workflows and 3858 independent
coordinate/contributor/Fraction cases per profile. The installed public consumer
passed. Checks include exact reads and dirty support, typed actual-read closure,
raw/quiet NaN and zero rules, strided input, four fenv modes, changed-index cache
replanning, Empty, cancellation, work/state limits and failed-attempt diagnostics.
Focused compiler/dependency/fragments/resources units and independent scoped
reviews passed. Manual acceptance has no integration-test registration.
Specification status remains Proposed; no performance claim is inferred.
