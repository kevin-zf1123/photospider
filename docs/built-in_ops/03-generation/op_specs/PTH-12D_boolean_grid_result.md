---
spec_schema_version: 1
id: PTH-12D
status: Accepted
implementation_status: not_implemented
registration_status: backend_and_schema_gates
---

# PTH-12D: Explicit-grid polygon Boolean Result

Inherits [PTH-12](PTH-12_boolean_offset_contract.md),
[GEN-common](GEN_common_contract.md) and [PTH geometry](PTH_geometry_contract.md). This
is a distinct versioned member; concrete registry spelling remains a freeze gate. Same
version/profile means bit-identical public output; no backend substitution changes that
contract.

Inputs: two Polyline PathSets; outputs: grid-geometry Result and quantization/geometry
report. Parameters explicitly include grid spacing/scale and grid origin, operation,
fill rules, attribute policy and hard budgets. Numeric types and representable grid
range must be frozen with the backend; all inputs must be finite, grid spacing positive.

Quantize each input coordinate to the declared grid under a versioned rounding rule. The
rule, intermediate intersection rounding, overflow handling, degenerate-feature handling
and result canonicalization must be fixed before registration. They are technical gates,
not permission for backend-default or adaptive behavior. Report input quantization and
all subsequently permitted geometric changes. No result may be described as the exact
Boolean of the unquantized source.

Preserve grid-coordinate output without implicit conversion to floating coordinates. Do
not silently fall back from PTH-12C to this member. A backend/library name alone is
insufficient to define its geometry semantics or approve feature removal. Whole
execution and complete associated publication apply. Grid scale/origin and backend
algorithm version participate in identity; changing legal output needs a version.

Acceptance: half-grid ties, signed coordinates, range/overflow limits, narrow gaps,
small holes, deterministic topology, exact grid round-trip and hard-budget failures.
Compare against the declared quantized algorithm, not an unrelated exact-input oracle.

These are specification acceptance requirements, not completed execution evidence.
