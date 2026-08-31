# Architecture Decision Records

English ADRs are authoritative. Each accepted ADR has a reader-oriented
Chinese mirror under [`zh/`](zh/README.zh.md). ADRs record decisions and target
contracts; current implementation facts and live delivery status remain in the
kernel architecture documents and issue/project tracking respectively.

| ADR | Decision |
| --- | --- |
| [0001](0001-graph-state-access-is-not-scheduler-dispatch.md) | Graph-state access is not scheduler dispatch. |
| [0002](0002-external-libraries-are-kernel-adapters.md) | External libraries are kernel adapters. |
| [0003](0003-process-owned-execution-resources.md) | Execution resources are process-owned. |
| [0004](0004-opencv-cpu-operations-are-reentrant-provider-work.md) | OpenCV CPU operations are reentrant provider work. |
| [0005](0005-graph-document-ingestion-is-a-classified-transaction.md) | Graph-document ingestion is a classified transaction. |
| [0006](0006-kernel-documentation-separates-facts-decisions-targets-and-status.md) | Kernel documentation separates facts, decisions, targets, and delivery status. |
| [0007](0007-compute-runs-and-process-execution-have-separate-owners.md) | Compute Runs and process execution have separate owners. |
| [0008](0008-generic-values-memory-bindings-and-regions-are-explicit-versioned-contracts.md) | Generic values, memory bindings, and regions are explicit versioned contracts. |
| [0009](0009-compute-io-durability-and-completion-semantics.md) | Compute I/O durability and completion are separate contracts. |
| [0010](0010-execution-profile-slos-are-six-independent-benchmark-verdicts.md) | Execution-profile SLOs are six independent benchmark verdicts. |
| [0011](0011-server-control-plane-workers-and-plugin-runtimes-are-separate-security-domains.md) | Server control plane, workers, artifact authority, and plugin runtimes are separate security domains. |
| [0012](0012-operation-plugins-use-a-separately-versioned-pure-c-abi.md) | Operation plugins use a separately versioned pure-C ABI. |
| [0013](0013-ordinary-dense-image-coordinates-samples-colors-and-statistics-are-separate-contracts.md) | Ordinary DenseImage coordinates, samples, colors, and statistics are separate contracts. |
| [0014](0014-compiler-document-and-plan-versions-are-independent.md) | Compiler, document, plan, digest, cache, and extension versions are independent contracts. |

New ADRs use the next four-digit number. A superseded ADR remains in this
sequence and links to its replacement rather than being silently rewritten or
deleted.
