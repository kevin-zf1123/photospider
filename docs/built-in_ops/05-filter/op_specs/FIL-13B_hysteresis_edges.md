---
spec_schema_version: 1
id: FIL-13B
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
parent_id: FIL-13
function: hysteresis_edges
proposed_operation_keys:
- filter.hysteresis_edges_strict
numeric_reference: discrete exact
oracle_scope: mathematical_reference
research_sources:
- S06
---

# FIL-13B: hysteresis_edges

Weak/strong edge connectivity.

Inherits [FIL-13](FIL-13_contract.md), the [FILTER common contract](FILTER_common_contract.md), [NUM](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT](../../02-format-color/op_specs/FMT_common_contract.md).

## Interface

Operation keys: `filter.hysteresis_edges_strict`.

`weak,strong` are same-shaped UInt8 inputs with values 0/1 → `edges` UInt8.

### Parameters

Axes are explicit; connectivity=4|8; callers guarantee binary mask encoding and strong⊆weak over the full domain without a separate validation scan.

## Mathematics

Numerical class: **discrete exact**；E/B/S notation is defined in [FILTER_numeric_reference](FILTER_numeric_reference.md).

Retain pixels in the weak graph that share a connected component with at least one strong pixel; set all others to zero. Adjacency is restricted to the original image: no wrap/reflect and no traversal through mask value 0.

## Behavior

The output depends on the complete plane of both `weak` and `strong`, per plane.

Connectivity results are unique and traversal order does not affect the output.

## Acceptance

weak=[1,1,1,0,1], strong=[1,0,0,0,0] → [1,1,1,0,0]; diagonal connectivity differs between 4 and 8; strong⊄weak violates the caller premise.

## References

[S06 · scikit-image feature](../research-sources.md#s06)
