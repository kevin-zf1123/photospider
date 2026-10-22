---
spec_schema_version: 1
id: PTH-01B
parent_id: PTH-01
function: concat_paths
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - path.concat_paths_v1_strict
---

# PTH-01B: concat paths

Inherit [PTH-01](PTH-01_construction_contract.md), [GEN common](GEN_common_contract.md),
and the applicable [random](NOI_random_contract.md) and
[geometry](PTH_geometry_contract.md) contracts. A key includes algorithm version and profile; within one version/profile,
output bits and discrete decisions are fixed.

## Interface and representation

K>=1 PathSet inputs, fixed compile-time arity; one PathSet.

## Parameters and domain

Identical coordinate systems and geometry authority; explicit conversion for mixed
authorities; attribute_policy default reject_unmappable.

## Mathematical specialization

Concatenate subpaths in input-port order without connecting equal endpoints. Reindex
offsets and associations. Path-constant attributes merge only with identical
values/descriptions. Arc-domain bindings must map using actual source lengths and
explicit parameter domain; otherwise reject or explicitly drop.

The v1 geometry schema contains no attached attribute members. These geometry-only
ports do not consume separate width or source-map Results. Workflows carrying such
Results must apply the association rules explicitly; selecting an attribute policy
does not copy a separate Result implicitly.

## Dependencies, resources and failures

Execution support: **complete input**. Lower the complete mathematical input
support and required output member tiles into explicit tasks. Share global work
through private intermediate Results and fixed dependencies. Required members
publish on successful call retirement. Empty output selection performs static
preparation without payload tasks; there is no runtime dependency discovery or
dirty propagation.

complete-input O(total rows), plus required length/mapping work.

All output, scratch, indices, algorithmic work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve static structural failures and actual
resource, cancellation, backend and upstream statuses. Member-specific no-solution,
refinement or quality failures apply to valid inputs and the explicitly declared
algorithm; numerical-premise violations have no prescribed runtime diagnostic.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

Equal endpoints remain separate subpaths, empty inputs, incompatible authority,
concat/split geometry roundtrip, unmappable attributes.

Check selected-tile/full-output equivalence, source-grid read closure, legal
strides/offsets, separate/joint outputs, version/profile identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. No unverified GPU or third-party bit
identity is implied by these requirements.
