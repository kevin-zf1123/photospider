---
spec_schema_version: 1
id: NUM-10D
parent_id: NUM-10
function: scatter_sum
proposed_operation_keys:
  - array.scatter_sum_strict
  - array.scatter_sum_accelerated_apple_silicon
  - array.scatter_sum_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_manual_acceptance
repository_branch: ops-specs
repository_commit: 30478d33
---

# NUM-10D: scatter_sum

Inherit the [NUM baseline](NUM_common_contract.md) for specification status,
registration, shared execution and acceptance requirements; explicit rules below
and in the named family contract take precedence.

Inherit the [scatter contract](NUM-10_scatter_contract.md) in full: base/indices/
updates port order, static axis, same value dtype/non-axis dimensions, four
supported dtypes, generic dense output, global index validation, exact contributor
support, index invalidation, errors, resources and immutable ownership. All three
profiles produce identical bits. Input base is never modified.

## Update semantics

Sum base and all matching updates as exact mathematical numbers, with one final
round-to-nearest/ties-even conversion to Float32/Float64 or final UInt8/Int64
range check. Do not round or reject intermediate partial sums. Integer overflow
is based only on the final result; floating overflow returns signed infinity.
Use an exact accumulator or certified equivalent, with actual limb/work capacity
charged to host budgets. No unordered floating atomic accumulation is allowed.

If no update matches, copy base without arithmetic, including sNaN bits. For
aggregate variants with matching updates, use the shared exceptional-value
priority and zero rules. Unrequested base/update values are not evaluated;
indices remains globally validated even for a partial output request.

## Acceptance and implementation status

Conceptual public fixture: base=[10,20,30], indices=[1,1,2], updates=[2,3,4],
axis=0 -> [10,25,34]. Use independent index grouping and the specified exact
numeric or bit-selection oracle; verify each port's actual read footprint.
Include duplicated targets, unhit sNaN base, selected and unselected failures,
NaN payload precedence, infinities, signed zeros, integer extrema, source strides,
index changes, global invalid-index rejection and shared resource/lifetime tests.

For Int64, base=INT64_MAX with matching updates [1,-1] must return INT64_MAX
without a partial-sum overflow failure. The same grouping principle applies to
finite floating cancellation; compare exact sums with one final rounding.

The current three profile keys use `scatter_sum_node` from
`photospider/numeric/indexing.hpp`. Matching contributors are accumulated by
the exact aggregate workspace and rounded once for floating output; Int64 uses
final range checking. The public fixture produces `[10,25,34]` and checks exact
contributor support, exceptional values, overflow attribution and cleanup.
On 2026-09-14, local AppleClang 21 strict/Apple and Ubuntu WSL Clang 18
strict/AVX2 passed the complete manual workflows and 3858 independent
coordinate/contributor/Fraction cases per profile. The installed public consumer
passed. Checks include exact reads and dirty support, typed actual-read closure,
raw/quiet NaN and zero rules, strided input, four fenv modes, changed-index cache
replanning, Empty, cancellation, work/state limits and failed-attempt diagnostics.
Focused compiler/dependency/fragments/resources units and independent scoped
reviews passed. Manual acceptance has no integration-test registration.
Specification status remains Proposed; no performance claim is inferred.
