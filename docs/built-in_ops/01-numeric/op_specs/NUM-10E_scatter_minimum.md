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
implementation_status: implemented
---

# NUM-10E: scatter_minimum

Inherits the [scatter contract](NUM-10_scatter_contract.md) for ports, shape/dtype rules, unchecked index premises, numerical classification and the base-axis semantics. Each input Result contains one tensor; output `values` uses ordinary base axes in `photospider.tensor`/`samples`.

## Mathematics

The result is the numerical minimum of the base and matching updates. The first source NaN is quieted with its payload preserved. Mixed signed zeros select -0. Integer comparisons remain integer comparisons. If no update hits a coordinate, the base bits are copied without arithmetic, including signaling NaNs. Base is not modified.

## Behavior

Each selected output sample depends on its base sample and on all update positions along the scatter axis for its non-axis region, plus the complete indices vector. Indices and normalized positions satisfy the unchecked caller premises. Empty selection reads nothing.

## Acceptance

Base `[10,20,30]`, indices `[1,1,2]`, updates `[2,3,4]`, axis 0 gives `[10,2,4]`. Cases: duplicate order, no-hit signaling NaNs, first-NaN precedence, infinities, zero signs, integer boundaries, partial/empty output.
