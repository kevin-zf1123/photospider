---
spec_schema_version: 1
id: GEN-06B
parent_id: GEN-06
function: lookup_numeric_gradient
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - generation.lookup_numeric_gradient_v1_strict
  - generation.lookup_numeric_gradient_v1_accelerated_apple_silicon
  - generation.lookup_numeric_gradient_v1_accelerated_x86_64
---

# GEN-06B: lookup numeric gradient

Inherits [GEN-06](GEN-06_gradient_lookup_contract.md) and [GEN common](GEN_common_contract.md).

## Interface

Operation keys: `generation.lookup_numeric_gradient_v1_strict`, `generation.lookup_numeric_gradient_v1_accelerated_apple_silicon`, `generation.lookup_numeric_gradient_v1_accelerated_x86_64`.

t with shape S and table[N,C], N>=2,C>=1; values S+[C], rank<=8.

### Parameters and domain

dtype default Float64; t in [0,1]; explicit upstream spread; raw numeric table.

## Mathematics

x=t*(N-1) exactly. Integer x selects that row with copy semantics; last row never accesses N. Otherwise j=floor(x), w=x-j, return RN_T((1-w)*table[j]+w*table[j+1]). No gamma or alpha handling.

## Behavior

Support class: Regional.

Table validity is an unchecked caller premise. Only the one or two selected rows contribute to a sample.

## Acceptance

Two-entry midpoint, signed-zero endpoint copy, C=4 remains raw numeric.
