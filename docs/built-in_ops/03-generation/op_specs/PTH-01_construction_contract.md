---
spec_schema_version: 1
id: PTH-01
kind: shared_operator_contract
category: 03-generation
status: Accepted
implementation_status: not_implemented
---

# PTH-01: Path construction and structure

Consume explicit typed geometry and associations, not SVG text. CoreVerbs and Primitives use separate schemas in the logical PathSet family; conversion is explicit. Complete delivery requires the lowering to write all required members before successful call retirement; CompleteBundle schema metadata does not create that coverage automatically. Dynamic row counts do not create dynamic port counts. Correct attribute mappings are required;
default reject_unmappable, explicit drop reports removed keys.

## Members

| ID | Specialization |
| --- | --- |
| [PTH-01A](PTH-01A_make_core_path.md) | make core path |
| [PTH-01B](PTH-01B_concat_paths.md) | concat paths |
| [PTH-01C](PTH-01C_split_subpaths.md) | split subpaths |
| [PTH-01D](PTH-01D_reverse_paths.md) | reverse paths |
| [PTH-01E](PTH-01E_make_primitive_path.md) | make primitive path |

## Shared execution and numerical rules

Inherit [GEN common](GEN_common_contract.md), and applicable
[random](NOI_random_contract.md) and [path geometry](PTH_geometry_contract.md) rules.
Members declare Regional, Halo or complete-input from actual mathematical dependencies,
independently of static structure checks. GPU capability is member/backend-specific.
Same version/profile means bit-identical output; numerical error, geometry error and
topology guarantees are separate. Source associations are unchecked caller premises; producers preserve declared ownership and lifetime by construction.

## Acceptance and implementation boundary

Use member-specific independent fixtures plus actual public workflow execution. The
[oracle coverage](../oracle-coverage.md) identifies finite exact helpers, Measured
diagnostics and remaining unsupported cases. It cannot establish public schema support,
GPU admission or complete domain certification. The specified schemas and RNG mappings require implementation and validation before registration. Unresolved numerical solver or backend choices remain explicit member limitations.
