---
spec_schema_version: 1
id: GEN-03D
parent_id: GEN-03
function: star_path
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - generation.star_path_v1_strict
---

# GEN-03D: star path

Inherits [GEN-03](GEN-03_basic_shapes_contract.md) and [GEN common](GEN_common_contract.md).

## Interface

Operation keys: `generation.star_path_v1_strict`.

center[2], radii[2]=(inner,outer), rotation[1]; closed CoreVerbs PathSet.

### Parameters and domain

points n=3..4096; rotation in radians; 0<inner<=outer.

## Mathematics

For k=0..2n-1 use theta=rotation+k*pi/n and alternating outer/inner radius. Publish RN64(center+r*(cos(theta),sin(theta))) and M/L/Z connectivity. Positive rotation is clockwise in y-down coordinates.

## Behavior

Support class: complete input.

## Acceptance

n=4 starts on positive x at rotation=0; equal radii form a regular 2n-gon; no epsilon angle snapping.
