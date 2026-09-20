---
spec_schema_version: 1
id: NUM-10C
parent_id: NUM-10
function: scatter_replace
proposed_operation_keys:
  - array.scatter_replace_strict
  - array.scatter_replace_accelerated_apple_silicon
  - array.scatter_replace_accelerated_x86_64
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

# NUM-10C: scatter_replace

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
supported dtypes, generic dense output, global index validation, numerical contributor
selection, index invalidation, errors, resources and immutable ownership. All three
profiles produce identical bits. Input base is never modified.

## Update semantics

Select the largest matching j and copy that update bit-for-bit. The
overwritten base and earlier matching updates do not enter numerical selection. There is no numeric
NaN quieting, arithmetic overflow or aggregation on a selected update.

If no update matches, copy base without arithmetic, including sNaN bits. For
aggregate variants with matching updates, use the shared exceptional-value
priority and zero rules. Whole reads and validates complete base/updates/indices, while only the
specified contributors enter arithmetic. Unselected upstream/typed failures
can fail the Run. The complete output is computed before consumer projection.

## Acceptance and implementation status

Conceptual public fixture: base=[10,20,30], indices=[1,1,2], updates=[2,3,4],
axis=0 -> [10,3,4]. Use independent index grouping and the specified exact
numeric or bit-selection oracle; verify each port's actual read footprint.
Include duplicated targets, unhit sNaN base, selected and unselected failures,
NaN payload precedence, infinities, signed zeros, integer extrema, source strides,
index changes, global invalid-index rejection and shared resource/lifetime tests.

All formal profile keys use CPU Whole. Current public workflows, independent
coordinate/contributor/Fraction oracles, failure/resource checks and performance
are in [NUM-10 Whole execution](../indexing-whole.md). Earlier 2026-09-14
regional strict/Apple/WSL and installed checks predate this migration.
