# 01-numeric implementation work

The current FP32 quality contract, backend, measured results and limitations are
maintained in the [accelerated specification](op_specs/NUM_accelerated_contract.md#current-implementation).
Formal complete-input profiles, including the fifteen LUT3D baking geometry
keys, use explicitly prepared shared stages. Their static mapper grants the
active inputs before execution; requested output tiles read the shared result.
The original numerical callback runs once per call activation, including a new
activation on a loop backedge. Sparse output selection retains the algorithm's
complete input and internal output requirements. Empty selections omit pure
output work while preserving any control flow and mandatory effects.

The four structured LUT3D Result keys use the current Result continuation
protocol. Pack and Measure use shared stages; Unpack and Gate consume fixed
input grants directly. Measurement uses bounded row windows within its complete
grants. It does not extend input dependencies between polls. The authoring
templates continue to expand ordinary nodes with independent output identities.
Legacy unsuffixed sample keys remain separate; pointwise helpers use explicit
pointwise mappings where registered.

Complete-input policy determines storage, invalidation and failure scope as
specified per family. Numerical formulas and profile error contracts are
unchanged. See [mathematical implementation](math-implementation.md),
[LUT3D baking](op_specs/CRV-09_bake_lut3d.md#maintained-implementation-and-validation)
and the [static mapping contract](../../kernel-architecture/Dependency-Data.md#static-mapping-contract).

Primitive keys and authoring templates
retain the distinctions in their individual specifications. Specification
acceptance and implementation completion remain separate states.
