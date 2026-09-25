---
spec_schema_version: 1
id: PTH-12E
status: AcceptedDesign
implementation_status: not_implemented
registration_status: backend_and_schema_gates
---

# PTH-12E: Publish exact geometry as Float64 PathSet

Inherits [D12](../decisions.md), [GEN-common](GEN_common_contract.md) and
[PTH geometry](PTH_geometry_contract.md). This is a distinct versioned member;
concrete registry spelling remains a freeze gate. Same version/profile means
bit-identical public output; no backend substitution changes that contract.

Input: exact polygon Result from PTH-12C or a formally compatible exact consumer.
Outputs: Float64 CoreVerbs PathSet and publication report with source association.
Parameters: epsilon_publication_px > 0 (candidate default 1e-9), attribute_policy
(default reject_unmappable), validation/work/capacity limits.

Canonicalize exact contours, round each rational coordinate once with RN64,
canonicalize mathematical zero to +0, then verify the actual published geometry.
Bound corresponding-edge displacement by epsilon_publication_px and prove required
face/edge incidence and absence of new crossings/merges/collapsed features.
Failure is InvalidQuality; arithmetic overflow and host resource/cancellation statuses
remain distinct. Do not snap, delete holes, silently loosen epsilon or return a best guess.
Output is the rounded geometry; its later consumers do not retain implicit exact history.

Whole input validation and complete Result publication apply. Exact owner references,
publication scratch, predicates and output associations are charged and lifetime-safe.
PTH-12A must produce the same published coordinates/report semantics as exact Boolean
followed by this member, with equivalent explicit parameters and canonicalization.

Acceptance: exact dyadic identity, non-dyadic rational rounding, sub-ULP collisions,
new crossings after rounding, bounds validation, output association and retained owner.

These are specification acceptance requirements, not completed execution evidence.
