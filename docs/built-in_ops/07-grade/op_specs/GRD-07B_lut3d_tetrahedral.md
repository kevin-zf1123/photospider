---
spec_schema_version: 1
id: GRD-07B
parent_id: GRD-07
function: lut3d_tetrahedral
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.lut3d_tetrahedral
---

# GRD-07B: Tetrahedral RGB LUT

Inherits the [family contract](GRD-07_lut3d_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.lut3d_tetrahedral` performs the transformation below.

## Interface

`input: T[S,C]`, `table: Float32/64[Nr,Ng,Nb,3]`, `axes: Float64[3,2]` with rows [start,end] -> `values: T[S,C]`.

Each extent is 2..256. Axis endpoints are distinct; ascending and descending axes are legal. Select a complete RGB group and specify its destination RGB description. edge=reject|clamp, default reject.

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

## Mathematics

For axis length N define u=(x-start)*(N-1)/(end-start). Clamp u to [0,N-1] in clamp mode; reject mode requires that interval. Choose i=min(floor(u),N-2), f=u-i. Sort local fractions in decreasing order with R,G,B tie priority. For a>=b>=c, use weights (1-a,a-b,b-c,c) on vertices 000, e_first, e_first+e_second, 111. Sum the weighted RGB vertices. Exact grid-vertex queries select that vertex. The LUT itself defines any change of RGB interpretation.

## Behavior

Each output uses the source query at that coordinate and the contributing control-table entries. The complete auxiliary table is an admitted static support superset. A source edit is local; table edits affect observations whose declared support includes the table. Empty and bypass-only requests have no table payload dependency.

## Acceptance

An identity LUT reproduces in-domain RGB within tolerance; a channel-swap LUT exchanges R and B. Check descending axes and a nonseparable interior query.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
