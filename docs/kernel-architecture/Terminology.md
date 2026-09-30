# Kernel Terminology

| Term | Meaning |
| --- | --- |
| `WorkflowDocument` | Caller-owned source graph used as compiler input. |
| `GraphContext` | Owner of a copied workflow document and its current revision. |
| `GraphSnapshot` | Coherent source and revision capture used by compilation. |
| `SemanticGraphIR` | Immutable, inferred operation graph produced by analysis. |
| `OptimizedGraphIR` | Distinct semantics-equivalent stage; current optimizer copies the semantic graph. |
| `ExecutionPlan` | Dependency-ordered local operations, placement choices, and propagated demands. |
| `ExecutionContext` | Owner of bounded workers, frozen operation definitions, caches, and resource accounting. |
| `ExecutionRun` | Private state for one synchronous execution call. |
| `ExecutionBindings` | Exact-name per-run input owners: `Value`, `RegionalSource`, `InputSnapshot`, or `PlanarImage`. |
| `Value` | Immutable logical descriptor and Region, affine byte layout, facets, and shared storage owner. |
| `Region` | Logical subset in descriptor-axis coordinates; it does not describe bytes. |
| `PlanarImage` | Structural image storage with explicit axes, component groups, and controlled sample access. |
| operation traits | Frozen metadata that declares input/output contracts, resource bounds, and backend capabilities. |
| operation registry | Frozen set of trusted operation definitions used to compile and execute plans. |
| digest / cache key | Non-security identity used for reproducibility checks or disposable cache lookup. |
| cancellation | Cooperative request that prevents successful late result publication. |
| CPU fallback | A new CPU attempt after an optional native callback reports backend unavailability before publishing output, when the operation traits permit it. |

`SessionId`, `JobId`, daemon IPC, and process lifecycle belong to `photospider-daemon`; its Jobs are ephemeral. Persistent results and recovery are outside the current product boundary. Kernel results remain in-memory values whose backing lifetime follows their owners.
