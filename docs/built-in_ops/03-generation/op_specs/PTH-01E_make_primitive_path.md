---
spec_schema_version: 1
id: PTH-01E
parent_id: PTH-01
function: make_primitive_path
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - path.make_primitive_path_v1_strict
---

# PTH-01E: make primitive path

Inherits [PTH-01](PTH-01_construction_contract.md) and [GEN common](GEN_common_contract.md).

## Interface

Operation keys: `path.make_primitive_path_v1_strict`.

The 13 field tensors of photospider.ops.path_primitives v1, including counts and subpath topology, produce one Primitives PathSet. Width and source-map Results remain separate workflow values.

### Parameters and domain

Explicit tag/record/payload-field associations; the separate Primitives schema contains no CoreVerbs fields; spline degree<=16.

## Mathematics

Caller premises are nonzero sub-full-turn ArcSweep and explicit FullTurn direction, nondegenerate ellipse axes, zero unused Bezier slots, positive rational weights and nondecreasing valid knots. Exact connections are also caller premises; no input scan or connection proof runs during publication or consumption. Representation conversion remains explicit.

## Behavior

Support class: complete input.

## Acceptance

Full circle is not zero sweep; degree0 spline; discontinuous knots require separate subpaths; valid ObjectId associations and in-range references.
