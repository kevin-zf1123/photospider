---
spec_schema_version: 1
id: PTH-07A
parent_id: PTH-07
function: trim_path
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - path.trim_path_v1_strict
---

# PTH-07A: trim path

Inherits [PTH-07](PTH-07_trim_dash_contract.md) and [GEN common](GEN_common_contract.md).

## Interface

Operation keys: `path.trim_path_v1_strict`.

Core/Bezier PathSet,start[1],end[1]; trimmed CoreVerbs PathSet with source mappings.

### Parameters and domain

units=normalized_arc/arc_px; each subpath range is valid as an unchecked premise; closed_wrap default false; attribute_policy.

Outputs: `path`, `source_map` and `report`. Int64 parameters: `max_output_segments` (default 4096), `max_refinements` (default 4096), `limb_capacity` (default 128).

The source map is an exact rational affine parameter map. If a true-arc cut needs an unrepresentable irrational mapping, the default `attribute_policy=reject_unmappable` fails; explicit `drop_unmappable` omits that mapping and reports the drop. A rounded cut parameter is never encoded as an exact source correspondence. Rational inverse parameters are retained exactly even when non-dyadic: for a monotone collinear polynomial cut the parameter `1/3` is representable. Only a proved irrational root enters the unmappable policy; an exhausted rationality certificate fails refinement.

## Mathematics

Convert endpoints to true subpath arc positions. a=b yields empty. Ordinary0<=a<=b<=L; closed explicit wrap with a>b emits[a,L] then[0,b] as one open subpath. Full range preserves original closed state/geometry bits. Invert boundaries, exact de Casteljau trim then RN64 controls; connection and publication geometry are unchecked caller premises, with no completed-output validation. Preserve source-position width using source snapshot and original lengths; explicit rebind alone reapplies full profile.

### Source width mapping

For every output fragment retain the source snapshot, subpath and ordered source arc intervals. Before publication, local fragment arc s maps to source a+s; a wrapped fragment uses [a,L] then [0,b]. After rounded-control publication the exact source parameter mapping is retained rather than equating new arc length with old arc length. A per-subpath pixel-domain query uses source s; a normalized subpath query uses s/source_L; a whole-PathSet normalized query uses (source_prefix+source_s)/source_total_L. Workflow-paired path_width Results and independent width inputs use the same mapping. If the formal representation cannot express it, reject_unmappable fails; explicit drop removes only unmappable fields and reports their keys and reasons. No interpolation is guessed and no rebind is implicit.

## Behavior

Support class: complete input.

Source-map reject/drop applies only when the selected map representation cannot express the computed mapping, such as a non-rational arc cut in the v1 rational affine map. Rounding compatibility, connections and topology remain unchecked premises; there is no publication scan or guessed interpolation.

## Acceptance

100px line25..75, empty interval, closed wrap, full-range bit identity and three width domains. Include diagonal irrational-length junction cuts and exact `t=1/3` maps for the monotone quadratic `C(t)=-1+3.5t-1.5t^2` and cubic `C(t)=27t^3`; distinguish these from proved irrational cuts.
