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

Inherits [PTH-12](PTH-12_boolean_offset_contract.md),
[GEN-common](GEN_common_contract.md) and [PTH geometry](PTH_geometry_contract.md). This
is a distinct registered versioned member. Same
version/profile means bit-identical public output; no backend substitution changes that
contract.

Input: exact polygon Result from PTH-12C or a formally compatible exact consumer.
Outputs: Float64 CoreVerbs PathSet and publication report with source association.
Parameters: epsilon_publication_px > 0 (default 1e-9), attribute_policy
(default reject_unmappable), work/capacity limits.

Canonicalize exact contours, round each rational coordinate once with RN64, canonicalize
mathematical zero to +0, and accumulate the Euclidean vertex-displacement bound
while constructing the output, using the exact squared-error and RU64 rule in
[PTH geometry](PTH_geometry_contract.md#filled-regions-boolean-and-distance). Finite representability, corresponding-edge displacement
within epsilon_publication_px, preserved face/edge incidence and absence of new
crossings/merges/collapsed features are unchecked caller preconditions. Do not scan the
rounded geometry or return InvalidQuality for violating those premises. The report
records the constructed displacement bound and topology_status=0 (unverified).
Work/capacity exhaustion and host resource/cancellation statuses remain operational
failures. Do not snap, delete holes or silently loosen epsilon.
Output is the rounded geometry; its later consumers do not retain implicit exact
history.

`path_boolean.cpp` implements the exact codec and RN64/RU64 construction stages. The algorithm reads the complete valid exact geometry and constructs a complete Result without an input or publication validator. Exact owner references,
publication scratch and output associations are charged and lifetime-safe.
PTH-12A and exact Boolean followed by this member produce equivalent published coordinates, error stages and topology outcomes under equivalent explicit parameters and canonicalization. Provenance records each operation's actual inputs: PTH-12A has two original source IDs, while this member has the single exact-geometry input ID. Their source-ID shape, values and complete report bytes need not match; no ancestry lookup or additional binding stage is required.

Acceptance: exact dyadic identity, non-dyadic rational rounding, small representable
features satisfying the publication premises, construction-time displacement reporting,
output association and retained owner. Out-of-domain collisions and new crossings do
not impose runtime rejection requirements.

The strict CPU implementation is registered. The acceptance list defines required behavior; finite validation does not establish every valid-input success domain or a GPU backend.
