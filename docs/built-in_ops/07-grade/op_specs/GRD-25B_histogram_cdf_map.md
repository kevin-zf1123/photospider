---
spec_schema_version: 1
id: GRD-25B
parent_id: GRD-25
function: histogram_cdf_map
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.histogram_cdf_map
---

# GRD-25B: Histogram CDF mapping

Inherits the [family contract](GRD-25_histogram_equalize_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.histogram_cdf_map` performs the transformation below.

## Interface

`counts: Int64[B]` in binary mode or `Float64[B]` in weighted mode -> `map: Float64[B]`, `identity: UInt8[1]`.

selection_mode=binary|weighted defaults binary; 2<=B<=65536. Counts/masses are nonnegative; binary total<=2^40.

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

## Mathematics

N=sum(counts). Let j0,j1 be first/last positive bins. If N=0 or j0=j1, identity=1 and map[i]=i/(B-1). Otherwise identity=0 and map[i]=max(0,(prefix[i]-counts[j0])/(N-counts[j0])).

## Behavior

Each map or flag depends on the complete count vector. identity=1 instructs the application to copy the domain-processed sample.

## Acceptance

counts=[1,1] gives map=[0,1],identity=0. Empty or single-occupied-bin histograms give identity=1.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
