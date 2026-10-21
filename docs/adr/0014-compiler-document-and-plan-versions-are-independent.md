# ADR 0014: Compiler Stages and Compatibility Versions Stay Distinct

- Status: Accepted

## Current implementation

Package 0.39.0 uses `ValidatedIR` for every validated IR version. Analysis, optional named optimization and named physical planning remain separate operations. Ordered Call IDs define retirement and effect order; task dependencies define issue order. The planner produces immutable physical Task/Tile/Call/Block tables. Explicit CFG loops retain activation semantics. See the [version contract](../development/Compiler-Version-Contract.md).

`.psir` and `.psxp` use format 5. Older artifacts require recompilation/re-export; there is no continuation Plan translation. Import validates the saved tables and rebinds static contracts/resources without running an optimizer, lowerer or planner. Device preparation is a later explicit operation.

## 1. Core summary

Document schema, operation declarations, plugin ABI, package API, artifact encoding and compiler identities describe different contracts. Changing one does not imply compatibility or a version change in the others. Optimization creates another immutable `ValidatedIR`; it is not a mandatory distinct public stage type.

## 2. Stage and identity model

Analysis produces validated source semantics. Optional named optimizers preserve required semantic order and return independently validated immutable versions. The planner accepts a legal validated version and records its actual input digest, execution mode, physical tables and selected returns. Planner name and version identify compilation diagnostics; planner selection remains a compilation option. The Plan stores the resulting physical contract rather than planner provenance.

IR content identity excludes provenance such as parent version and transformation history. Plan identity includes the physical execution contract; changing `CpuExact` to `NativeGpu` changes identity even when the saved tasks are all CPU. Preparation binds concrete devices and kernels without changing Plan topology or identity.

## 3. Compatibility axes

| Axis | Current contract |
| --- | --- |
| WorkflowDocument schema | 7 |
| Result schema encoding | 6 |
| OperationTraits | 30 |
| Operation module / fixed invocation C ABI | 8 / 8 |
| Native kernel service | 2 |
| IR / Plan artifact format | 5 / 5 |
| Package / SOVERSION | 0.39.0 / 0.39 |

The compiler encodes closed document fields, normalized parameters, copied operation contracts, static specialization and declared resource identities in the applicable content domain. Physical identity additionally includes schemas, layouts, tiles, placements, tasks, adjacency, call membership, CFG, selections and reservation bounds.

Each in-memory stage carries a private weak identity for its exact frozen operation registry. Optimizer, planner and execution preparation reject a foreign-registry stage even if keys match. This runtime identity, native addresses, allocation IDs, timings, cancellation, queue state and daemon identifiers are excluded from canonical digests and serialized data.

Float parameter identity preserves copied binary64 bits in fixed little-endian order, including signed zero and non-finite payloads accepted by schema validation. Callback addresses and opaque static-state pointers are excluded; their declared immutable contract/resource identities participate where specified.

Plans are validated in-memory values and versioned compiler artifacts, not daemon wire messages. A content digest identifies content, not trusted native code. Source-bound Plans retain graph-currentness checks; standalone imported Plans are source independent. Saving a Plan never saves Run state, payload caches or device handles.

## 4. Consequences

Consumers version each boundary they expose and recompile derived Plans when source semantics, operation contracts, planner behavior or selected placement changes. A saved artifact must pass bounded parsing, structural validation and contract linking before preparation. Package changes may break consumers while workflow schema remains unchanged. This decision does not grant cross-version ABI or artifact compatibility.
