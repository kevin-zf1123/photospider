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
| `ExecutionBindings` | Exact-name per-run input owners, each an owning `ResultRef` checked against its declaration schema. |
| `ResultRef` | Owns a static Tensor-only Result schema, captured coverage, backing owners and ObjectId provenance. |
| `Tensor` | A descriptor, exact authorized Footprint and owned TensorSpan fragments; the single numerical carrier. |
| operation metadata | `OperationMetadata`: the complete Result schema at an operation boundary. Specialization infers output structure from input structure and static parameters. |
| `metadata_owner` | Resource lease that pays for a schema's managed nested containers; budget ownership, not interpretation. |
| `ImmutableResource` / `ResourceSet` | Content-addressed immutable bytes (length and SHA-256) and an immutable set of them. Operations select resources by identity at preparation; plans retain them. |
| validator | Registered `ValidationDefinition` bound explicitly to a port by key, version, and implementation. It checks host-authorized Tensor samples. |
| diagnostic report | Fixed-size opaque record type registered as `DiagnosticDefinition` and merged by `DiagnosticAccumulator`. |
| `DomainErrorCode` | Opaque 64-bit failure code that the kernel stores in `FailureDetail::domain_code` without interpretation. |
| operations SDK | `Photospider::ops_sdk`, headers under `include/photospider/ops/`: color descriptions, domain codecs and arithmetic, description editing, LUT bake contract, quality and numeric reports, Layer types. Not part of the kernel. |
| `Region` | Logical subset in descriptor-axis coordinates; it does not describe bytes. |
| `TensorSpan` | One owned rectangular mapping inside Tensor; actual signed strides and backing ownership. |
| operation traits | Frozen metadata that declares input/output contracts, resource bounds, and backend capabilities. |
| operation registry | Frozen set of trusted operation definitions used to compile and execute plans. |
| digest / cache key | Non-security identity used for reproducibility checks or disposable cache lookup. |
| cancellation | Cooperative request that prevents successful late result publication. |
| CPU fallback | A new CPU attempt after an optional native callback reports backend unavailability before publishing output, when the operation traits permit it. |
| canonical image | An operation-domain convention: ordinary image operations default to straight alpha and host backing defaults to tiled planar. No Tensor conformance label or automatic validator is required; explicit association conversions remain available. |
| boundary payload | Premultiplied numeric tensor produced or consumed by FMT-04 adapters for export or external interfaces. The association is stated by the adapter's explicit parameters; the tensor itself carries no label, so a consumer relies on its own explicit parameters or bound validator. |

`SessionId`, `JobId`, daemon IPC, and process lifecycle belong to `photospider-daemon`; its Jobs are ephemeral. Persistent results and recovery are outside the current product boundary. Kernel results remain in-memory values whose backing lifetime follows their owners.
