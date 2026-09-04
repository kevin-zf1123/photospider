# 架构决策记录

英文 ADR 是权威来源。每个已接受 ADR 都在本目录中具有面向读者的中文镜像。ADR
记录决策与目标契约；当前实现事实与公开实时交付状态分别由 kernel 架构文档和
GitHub Issue 维护。GitHub Project 是这些 Issue 的 maintainer operational view。
私有 personal-overlay OpenSpec 文件属于 maintainer working note，不具有公开
architecture 或 delivery authority。

| ADR | 决策 |
| --- | --- |
| [0002](0002-external-libraries-are-kernel-adapters.zh.md) | 外库不进入 kernel 语义。 |
| [0003](0003-process-owned-execution-resources.zh.md) | 本地执行资源具有显式所有者。 |
| [0005](0005-graph-document-ingestion-is-a-classified-transaction.zh.md) | Workflow 文档摄取是分类事务。 |
| [0006](0006-kernel-documentation-separates-facts-decisions-targets-and-status.zh.md) | 文档分离事实、决策与交付状态。 |
| [0007](0007-compute-runs-and-process-execution-have-separate-owners.zh.md) | Compute Run 与本地执行资源具有不同所有者。 |
| [0008](0008-generic-values-memory-bindings-and-regions-are-explicit-versioned-contracts.zh.md) | Value、facet、layout 与 Region 是显式验证契约。 |
| [0012](0012-operation-plugins-use-a-separately-versioned-pure-c-abi.zh.md) | Operation 与 data provider 使用版本化进程内 ABI。 |
| [0014](0014-compiler-document-and-plan-versions-are-independent.zh.md) | Compiler document、IR、plan 与 digest 具有独立 identity。 |
| [0015](0015-breaking-product-boundary-scope-reset.zh.md) | 产品边界是可嵌入 kernel 与临时 local daemon。 |

ADR 0015 是最高 active 产品边界权威。重置前 ADR 0001、0004、0009、0010、0011 与 0013 已由该
breaking decision 有意从 active 集合退役。其历史文本只能从 Git 历史和
`pre-breaking-scope-reset-2026-09-01` tag 取得，不得作为 roadmap 或恢复来源。

新 ADR 使用下一个四位编号。普通 supersession 会链接替代项；当保留旧决策会错误
宣传已删产品领域时，breaking scope reset 可以直接将 active decision 退役。
