# Architecture Decision Records

ADRs record durable architecture constraints and their rationale. Use them to understand why a boundary exists; use the [kernel architecture](../kernel-architecture/README.md) for current implementation behavior. English ADRs are authoritative, with reader-oriented Chinese mirrors under [`zh/`](zh/README.zh.md).

| ADR | Decision |
| --- | --- |
| [0002](0002-external-libraries-are-kernel-adapters.md) | Keep third-party libraries behind kernel contracts. |
| [0003](0003-process-owned-execution-resources.md) | Give local execution resources explicit owners. |
| [0005](0005-graph-document-ingestion-is-a-classified-transaction.md) | Publish workflow source separately from compiler stages. |
| [0006](0006-kernel-documentation-separates-facts-decisions-targets-and-status.md) | Keep current behavior documentation separate from architecture decisions. |
| [0007](0007-compute-runs-and-process-execution-have-separate-owners.md) | Separate per-run state from shared execution-context resources. |
| [0008](0008-generic-values-memory-bindings-and-regions-are-explicit-versioned-contracts.md) | Make values, input bindings, layouts, and Regions explicit contracts. |
| [0012](0012-operation-plugins-use-a-separately-versioned-pure-c-abi.md) | Use versioned C contracts for trusted operation and data-provider modules. |
| [0014](0014-compiler-document-and-plan-versions-are-independent.md) | Keep document, compiler-stage, plan, and ABI identities distinct. |
| [0015](0015-breaking-product-boundary-scope-reset.md) | Keep compilation and execution in the embeddable kernel. |
| [0016](0016-workflow-inputs-and-execution-bindings.md) | Bind declared workflow inputs for each execution. |
| [0017](0017-cpu-regional-execution-and-storage.md) | Execute regional work with bounded CPU storage. |
| [0018](0018-local-result-caches-and-frozen-execution.md) | Freeze execution inputs and reuse exact local results. |
| [0019](0019-metal-resident-image-workflows.md) | Keep supported image workflows resident in Metal storage. |
| [0020](0020-composable-operation-foundations.md) | Compose numeric, semantic, and output contracts across operations. |
| [0021](0021-independent-node-results.md) | Represent independent node results and joint execution explicitly. |

The [kernel overview](../kernel-architecture/Overview.md) states current ownership and execution behavior. Individual ADRs remain useful when their constraints apply; a decision record does not establish that a proposed or accepted capability is implemented.
