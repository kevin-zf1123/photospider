# 架构决策记录

英文 ADR 是权威来源。每个已接受 ADR 都在本目录中具有面向读者的中文镜像。ADR
记录决策与目标契约；当前实现事实与实时交付状态分别由 kernel 架构文档和
issue/project tracking 维护。

| ADR | 决策 |
| --- | --- |
| [0001](0001-graph-state-access-is-not-scheduler-dispatch.zh.md) | 图状态访问不是 scheduler dispatch。 |
| [0002](0002-external-libraries-are-kernel-adapters.zh.md) | 外部库是 kernel adapter。 |
| [0003](0003-process-owned-execution-resources.zh.md) | 执行资源由进程拥有。 |
| [0004](0004-opencv-cpu-operations-are-reentrant-provider-work.zh.md) | OpenCV CPU operation 是可重入 provider 工作。 |
| [0005](0005-graph-document-ingestion-is-a-classified-transaction.zh.md) | Graph 文档摄取是有分类的事务。 |
| [0006](0006-kernel-documentation-separates-facts-decisions-targets-and-status.zh.md) | Kernel 文档拆分事实、决策、目标与交付状态。 |
| [0007](0007-compute-runs-and-process-execution-have-separate-owners.zh.md) | Compute Run 与进程执行具有不同所有者。 |
| [0008](0008-generic-values-memory-bindings-and-regions-are-explicit-versioned-contracts.zh.md) | 通用 Value、内存 binding 与 Region 是显式版本化契约。 |
| [0009](0009-compute-io-durability-and-completion-semantics.zh.md) | Compute I/O durability 与完成语义是不同契约。 |
| [0010](0010-execution-profile-slos-are-six-independent-benchmark-verdicts.zh.md) | 执行画像 SLO 是六项独立 benchmark 判定。 |
| [0011](0011-server-control-plane-workers-and-plugin-runtimes-are-separate-security-domains.zh.md) | Server control plane、worker、artifact authority 与 plugin runtime 属于不同安全域。 |
| [0012](0012-operation-plugins-use-a-separately-versioned-pure-c-abi.zh.md) | Operation plugin 使用独立版本化 pure-C ABI。 |

新 ADR 使用下一个四位编号。被取代的 ADR 仍保留在序列中并链接到替代项，不能被
静默改写或删除。
