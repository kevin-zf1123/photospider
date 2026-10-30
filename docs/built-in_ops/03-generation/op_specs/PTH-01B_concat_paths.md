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

Inherits [PTH-01](PTH-01_construction_contract.md) and [GEN common](GEN_common_contract.md).

## Interface

Operation keys: `path.concat_paths_v1_strict`.

K>=1 PathSet inputs, fixed compile-time arity; one PathSet.

### Parameters and domain

Identical coordinate systems and geometry authority; explicit conversion for mixed authorities; attribute_policy default reject_unmappable.

## Mathematics

Concatenate subpaths in input-port order without connecting equal endpoints. Reindex offsets and associations. Path-constant attributes merge only with identical values/descriptions. Arc-domain bindings must map using actual source lengths and explicit parameter domain; otherwise reject or explicitly drop.

The v1 geometry schema contains no attached attribute members. These geometry-only ports do not consume separate width or source-map Results. Workflows carrying such Results must apply the association rules explicitly; selecting an attribute policy does not copy a separate Result implicitly.

## Behavior

Support class: complete input.

## Acceptance

Equal endpoints remain separate subpaths, empty inputs, incompatible authority, concat/split geometry roundtrip, unmappable attributes.
