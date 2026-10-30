---
spec_schema_version: 1
id: PTH-07
kind: shared_operator_contract
category: 03-generation
status: Accepted
implementation_status: implemented
---

# PTH-07: Trim and dash

Operate on Core/Bezier paths; other authorities require explicit conversion. True arc-length inversion is not uniform-t sampling. Preserve source-position width by default. An explicit rebind is required to spread the whole profile across each output fragment. Retain source parameter mappings even when control publication changes the new path length.

## Members

| ID | Specialization |
| --- | --- |
| [PTH-07A](PTH-07A_trim_path.md) | trim path |
| [PTH-07B](PTH-07B_dash_path.md) | dash path |

## Shared rules

Members inherit [GEN common](GEN_common_contract.md) and [PTH geometry](PTH_geometry_contract.md).
