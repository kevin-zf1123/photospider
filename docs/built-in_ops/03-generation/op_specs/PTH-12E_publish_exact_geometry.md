---
spec_schema_version: 1
id: PTH-12E
status: Accepted
implementation_status: implemented
registration_status: registered
operation_keys:
  - path.publish_exact_geometry_v1_strict
---

# PTH-12E: Publish exact geometry as Float64 PathSet

Inherits [PTH-12](PTH-12_boolean_offset_contract.md), [GEN common](GEN_common_contract.md) and [PTH geometry](PTH_geometry_contract.md).

## Interface

Operation keys: `path.publish_exact_geometry_v1_strict`.

Input: an exact polygon Result from PTH-12C or a formally compatible exact consumer. Outputs: a Float64 CoreVerbs PathSet and a publication report with source association. Parameters: `epsilon_publication_px` > 0 (default 1e-9), `attribute_policy` (default `reject_unmappable`).

## Mathematics

Canonicalize the exact contours, round each rational coordinate once with RN64, canonicalize mathematical zero to +0, and accumulate the Euclidean vertex-displacement bound while constructing the output, using the exact squared-error and RU64 rule in [PTH geometry](PTH_geometry_contract.md#filled-regions-boolean-and-distance). Finite representability, corresponding-edge displacement within `epsilon_publication_px`, preserved face/edge incidence and absence of new crossings, merges or collapsed features are unchecked caller preconditions; the rounded geometry is not scanned and no `InvalidQuality` is returned for violating them. The report records the constructed displacement bound and `topology_status=0` (unverified). Holes are not snapped or deleted and epsilon is not loosened.

The output is the rounded geometry; later consumers do not retain implicit exact history. PTH-12A and an exact Boolean followed by this member produce equivalent published coordinates, error stages and topology outcomes under equivalent explicit parameters and canonicalization. The provenance records each operation's actual inputs: PTH-12A has two original source IDs, while this member has the single exact-geometry input ID, so source-ID shape, values and complete report bytes need not match.

## Behavior

Support class: complete input. The member reads the complete valid exact geometry and constructs a complete Result.

## Acceptance

Exact dyadic identity, non-dyadic rational rounding, small representable features satisfying the publication premises, construction-time displacement reporting and output association.
