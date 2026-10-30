---
spec_schema_version: 1
id: PTH-01
kind: shared_operator_contract
category: 03-generation
status: Accepted
implementation_status: implemented
---

# PTH-01: Path construction and structure

Consume explicit typed geometry and associations, not SVG text. CoreVerbs and Primitives use separate schemas in the logical PathSet family; conversion is explicit. Dynamic row counts do not create dynamic port counts. Correct attribute mappings are required; default reject_unmappable, explicit drop reports removed keys.

## Members

| ID | Specialization |
| --- | --- |
| [PTH-01A](PTH-01A_make_core_path.md) | make core path |
| [PTH-01B](PTH-01B_concat_paths.md) | concat paths |
| [PTH-01C](PTH-01C_split_subpaths.md) | split subpaths |
| [PTH-01D](PTH-01D_reverse_paths.md) | reverse paths |
| [PTH-01E](PTH-01E_make_primitive_path.md) | make primitive path |

## Shared rules

Members inherit [GEN common](GEN_common_contract.md) and [PTH geometry](PTH_geometry_contract.md).
