# ADR 0006：文档将当前事实与架构决策分开

- 状态：已接受

## 1. 核心摘要 (TL;DR)
架构文档描述当前源码树中的行为。ADR 记录长期边界及其理由。区分两者后，工程师可以直接依据当前契约工作，无需重建交付历史。

## 2. 架构心智模型

```text
typed public headers + implementation --> current behavior documentation
accepted architecture constraints ------> ADRs
```

两种文档从不同角度描述同一产品：当前事实说明今天可以调用什么，ADR 说明长期边界为何存在。

## 3. 契约规约与接口

```cpp
// Public declarations and behavior are authoritative for current API facts.
#include <photospider/photospider.hpp>
```

`docs/kernel-architecture/` 记录已经实现的所有权、接口、不变量和限制。`docs/adr/` 记录已接受的设计、理由、后果和边界。实现变化时，当前事实文档应跟随检出代码中的头文件与实现；只有架构决策改变时才修改 ADR。英文文档是权威版本，维护中的中文镜像表达同一契约。

## 4. 非目标与明确边界
- ADR 不承担任务追踪、发布状态或路线图职责。
- 架构文档不声明实现尚不具备的行为。
- 私有工作笔记和外部 issue tracker 不是公共 API 的权威来源。

## 5. 后果与代价
读者可以判断一条陈述描述的是当前行为还是设计约束。维护者只需更新职责发生变化的文档，避免过期交付细节被误认为产品行为。若头文件与当前事实文档不一致，应先核对实现，再决定如何修正文档。
