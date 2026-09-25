---
spec_schema_version: 1
id: PTH-12F
status: AcceptedDesign
implementation_status: not_implemented
registration_status: backend_and_schema_gates
---

# PTH-12F: Explicit-grid Boolean with Float64 output

Inherits [D12](../decisions.md), [GEN-common](GEN_common_contract.md) and
[PTH geometry](PTH_geometry_contract.md). This is a distinct versioned member;
concrete registry spelling remains a freeze gate. Same version/profile means
bit-identical public output; no backend substitution changes that contract.

Inputs and quantized Boolean semantics match PTH-12D. Outputs are a Float64
CoreVerbs PathSet and both quantization/publication diagnostics.
Parameters include the same grid origin/scale and algorithm identity, plus a positive
publication displacement budget and separate hard validation/resource limits.

Execute the fixed grid algorithm, map grid coordinates to exact geometric coordinates
using the declared grid origin/scale, then round once to Float64. Verify the chosen
publication budget and the incidence/topology of the grid result after conversion.
Distinct grid points collapsing in Float64 cannot silently count as successful publication.
Do not claim original-input exactness. Canonical output must agree with an explicit
compatible grid-result publication path, once that codec/conversion is frozen.

Whole execution; no partial Result on failure. Tests include large grid indices,
non-power-of-two scale, topology-preserving and topology-breaking publication,
scale/origin identity, overflow and cancellation. Backend and codec remain gates.

These are specification acceptance requirements, not completed execution evidence.
