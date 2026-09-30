# 架构决策记录

ADR 记录长期有效的架构约束及其理由。了解边界为何存在时阅读 ADR；了解当前实现行为时阅读 [kernel 架构文档](../../kernel-architecture/zh/README.zh.md)。英文 ADR 为权威来源，中文镜像面向中文读者。

| ADR | 决策 |
| --- | --- |
| [0002](0002-external-libraries-are-kernel-adapters.zh.md) | 第三方库位于 kernel 契约之后。 |
| [0003](0003-process-owned-execution-resources.zh.md) | 为本地执行资源指定明确所有者。 |
| [0005](0005-graph-document-ingestion-is-a-classified-transaction.zh.md) | 分开发布 workflow 源文档与编译阶段。 |
| [0006](0006-kernel-documentation-separates-facts-decisions-targets-and-status.zh.md) | 将当前行为文档与架构决策分开维护。 |
| [0007](0007-compute-runs-and-process-execution-have-separate-owners.zh.md) | 分开每次运行的状态与共享执行上下文资源。 |
| [0008](0008-generic-values-memory-bindings-and-regions-are-explicit-versioned-contracts.zh.md) | 将 Value、输入绑定、布局和 Region 定义为显式契约。 |
| [0012](0012-operation-plugins-use-a-separately-versioned-pure-c-abi.zh.md) | 为可信 operation 与 data provider 模块使用版本化 C 契约。 |
| [0014](0014-compiler-document-and-plan-versions-are-independent.zh.md) | 区分文档、编译阶段、plan 与 ABI 的标识。 |
| [0015](0015-breaking-product-boundary-scope-reset.zh.md) | 将编译与执行保留在可嵌入 kernel 中。 |
| [0016](0016-workflow-inputs-and-execution-bindings.zh.md) | 每次执行都绑定已声明的 workflow 输入。 |
| [0017](0017-cpu-regional-execution-and-storage.zh.md) | 使用有界 CPU 存储执行区域工作。 |
| [0018](0018-local-result-caches-and-frozen-execution.zh.md) | 冻结执行输入，并复用精确匹配的本地结果。 |
| [0019](0019-metal-resident-image-workflows.zh.md) | 让受支持的图像工作流驻留于 Metal 存储。 |
| [0020](0020-composable-operation-foundations.zh.md) | 在算子间组合数值、语义和输出契约。 |
| [0021](0021-independent-node-results.zh.md) | 显式表示独立节点结果与联合执行。 |

[Kernel 概览](../../kernel-architecture/zh/Overview.zh.md)说明当前所有权和执行行为。各 ADR 在其约束适用时仍可供查阅；决策记录本身不能证明某项提议或已接受能力已经实现。
