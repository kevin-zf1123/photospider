---
spec_schema_version: 1
id: PTH-07B
parent_id: PTH-07
function: dash_path
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - path.dash_path_v1_strict
---

# PTH-07B: dash path

Inherits [PTH-07](PTH-07_trim_dash_contract.md) and [GEN common](GEN_common_contract.md).

## Interface

Operation keys: `path.dash_path_v1_strict`.

Core/Bezier PathSet,nonnegative pattern[K],finite offset[1] in px; dashed PathSet with source mappings.

### Parameters and domain

K>=1,at least one positive entry; restart=per_subpath; max_output_segments; attribute_policy.

Outputs: `path`, `source_map` and `report`, with the PTH-07A source-mapping policy and defaults (`max_output_segments` 4096, `max_refinements` 4096, `limb_capacity` 128). Every emitted fragment retains its original source position.

## Mathematics

Duplicate odd patterns, even entries on/odd off. P=sum(pattern), q=offset-P*floor(offset/P); phase=(s+q) mod P. Half-open intervals, skip zero entries without zero-length dashes. Closed seam-connected on pieces join into one open dash; only all-on retains closed. Dash boundaries are exact rational pixel-arc targets compared with true lengths, so a boundary that coincides with a segment junction or the full subpath length is decided exactly. Each piece uses PTH-07A and preserves original width positions, not a fresh normalized profile.

### Source width mapping

For every output fragment retain the source snapshot, subpath and ordered source arc intervals. Before publication, local fragment arc s maps to source a+s; a wrapped fragment uses [a,L] then [0,b]. After rounded-control publication the exact source parameter mapping is retained rather than equating new arc length with old arc length. A per-subpath pixel-domain query uses source s; a normalized subpath query uses s/source_L; a whole-PathSet normalized query uses (source_prefix+source_s)/source_total_L. Workflow-paired path_width Results and independent width inputs use the same mapping. If the formal representation cannot express it, reject_unmappable fails; explicit drop removes only unmappable fields and reports their keys and reasons. No interpolation is guessed and no rebind is implicit.

## Behavior

Support class: complete input. Hard limits never erase tiny dashes.

## Acceptance

L10 pattern3,2 gives[0,3],[5,8]; offset1 gives[0,2],[4,7],[9,10]; odd3 equals3,3; at least one positive pattern entry is assumed; closed seam.
