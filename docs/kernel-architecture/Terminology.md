# Kernel Terminology

| Term | Canonical meaning |
| --- | --- |
| `WorkflowDocument` | Caller-owned, format-neutral source graph. |
| `GraphContext` | One copied source document plus monotonic revision/currentness; never a daemon Session. |
| `GraphSnapshot` | Coherent source/revision capture whose currentness becomes false after replace/destruction. |
| `SemanticGraphIR` | Deterministic topological nodes with copied traits and inferred output descriptors. |
| `OptimizedGraphIR` | Distinct semantics-equivalent stage; currently a conservative no-op. |
| `ExecutionPlan` | Dependency-ordered local steps, backend labels, estimated bytes, named output demands, and derived per-input demands. |
| `ExecutionContext` | Owner of bounded CPU/GPU callback pools, frozen operations, and byte ledger. |
| `ExecutionRun` | Private state of one synchronous execute call; never a daemon Job or public identity. |
| `Value` | Immutable dense descriptor, Region, strided layout, bounded facets, and owned shared bytes. |
| `Region` | Rank-general logical subset of a Value shape; never a buffer or storage object. |
| operation traits | Copied compiler-visible input count, effects, backend/fallback, type/shape/Region rule, fixed shape, typed parameter schema, and estimated bytes. |
| operation parameter schema | Closed canonical key, exact source variant type, and required flag published before semantic IR. |
| operation/data-definition DSO | Startup-configured trusted in-process extension. |
| digest/cache key | Non-security reproducibility or disposable lookup identity. |
| cancellation | Cooperative observation that prevents late result publication. |
| fallback | Trait-permitted CPU attempt after an output-free optional GPU callback returns explicit backend unavailability. |

`SessionId`, `JobId`, Job status/result release, and daemon process lifecycle
belong only to `photospider-daemon`. Network service, tenant isolation, durable
work, worker processes, policy plugins, native-code security products, durable
result identity, and release evidence are removed or out of scope.
