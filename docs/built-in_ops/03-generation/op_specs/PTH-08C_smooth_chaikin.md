---
spec_schema_version: 1
id: PTH-08C
parent_id: PTH-08
function: smooth_chaikin
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - path.smooth_chaikin_v1_strict
---

# PTH-08C: smooth chaikin

Inherits [PTH-08](PTH-08_editing_contract.md) and [GEN common](GEN_common_contract.md).

## Interface

Operation keys: `path.smooth_chaikin_v1_strict`.

M/L/Z PathSet; smoothed Polyline PathSet.

### Parameters and domain

iterations0..12,max_vertices; open_endpoint_policy=preserve; internal locked corners require explicit splitting.

Defaults: one iteration and `max_vertices=1048576`.

## Mathematics

Each edge produces Q=(3P_i+P_(i+1))/4 and R=(P_i+3P_(i+1))/4, RN64 each round before next iteration. Preserve open endpoints; closed loops omit duplicate closure vertex. Zero iterations copy bits. This changes shape without an epsilon or topology guarantee.

## Behavior

Support class: complete input. No geometry-error or topology certificate is inferred from Chaikin smoothing.

## Acceptance

0->4 first round0,1,3,4; closed4 vertices become8; identity bits at zero rounds.
