---
spec_schema_version: 1
id: GEN-04G
parent_id: GEN-04
function: linear_rgb_bars
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - generation.linear_rgb_bars_v1_strict
---

# GEN-04G: linear rgb bars

Inherits [GEN-04](GEN-04_test_patterns_contract.md) and [GEN common](GEN_common_contract.md).

## Interface

Operation keys: `generation.linear_rgb_bars_v1_strict`.

No Tensor inputs; image[H,W,3] Float32/64.

### Parameters and domain

canvas/dtype; fixed linear sRGB/D65/relative, straight without alpha; no gain or legal-range scaling.

## Mathematics

j=min(7,floor(8*(x+1/2)/W)). Select white, yellow, cyan, green, magenta, red, blue, black in that order, using components 0/1.

## Behavior

Support class: Regional.

## Acceptance

W=8 gives one pixel per bar; W<8 can skip bars; no SMPTE/EBU compliance claim.
