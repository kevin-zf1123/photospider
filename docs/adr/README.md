# Architecture Decision Records

English ADRs are authoritative. Each accepted ADR has a reader-oriented
Chinese mirror under [`zh/`](zh/README.zh.md). ADRs record decisions and target
contracts; current implementation facts and public live delivery status remain
in the kernel architecture documents and GitHub Issues respectively. GitHub
Projects are maintainer operational views of those Issues.
Private personal-overlay OpenSpec files are maintainer working notes and carry
no public architecture or delivery authority.

| ADR | Decision |
| --- | --- |
| [0002](0002-external-libraries-are-kernel-adapters.md) | External libraries stay outside kernel semantics. |
| [0003](0003-process-owned-execution-resources.md) | Local execution resources are explicitly owned. |
| [0005](0005-graph-document-ingestion-is-a-classified-transaction.md) | Workflow-document ingestion is a classified transaction. |
| [0006](0006-kernel-documentation-separates-facts-decisions-targets-and-status.md) | Documentation separates facts, decisions, and delivery status. |
| [0007](0007-compute-runs-and-process-execution-have-separate-owners.md) | Compute Runs and local execution resources have separate owners. |
| [0008](0008-generic-values-memory-bindings-and-regions-are-explicit-versioned-contracts.md) | Value, facets, layout, and Region are explicit validated contracts. |
| [0012](0012-operation-plugins-use-a-separately-versioned-pure-c-abi.md) | Operations and data providers use versioned in-process ABIs. |
| [0014](0014-compiler-document-and-plan-versions-are-independent.md) | Compiler documents, IR, plans, and digests have separate identities. |
| [0015](0015-breaking-product-boundary-scope-reset.md) | The product boundary is an embeddable kernel and an ephemeral local daemon. |
| [0016](0016-workflow-inputs-and-execution-bindings.md) | Accepted Float32 image/scalar and operation ABI v3 target; implementation pending. |

ADR 0015 is the highest active product-boundary authority. Pre-reset ADRs 0001,
0004, 0009, 0010, 0011, and 0013 were deliberately retired from the active set
by that breaking decision. Their historical text remains available only in Git history and the
`pre-breaking-scope-reset-2026-09-01` tag; it must not be treated as a roadmap
or recovery source.

New ADRs use the next four-digit number. A normal supersession links its
replacement. A breaking scope reset may instead retire an active decision when
retaining it would incorrectly advertise a removed product domain.
