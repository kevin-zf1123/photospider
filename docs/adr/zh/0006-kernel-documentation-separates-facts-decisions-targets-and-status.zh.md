# ADR 0006：内核文档分开事实、决策、目标与实施状态

## 状态

已接受，并已应用到维护中的内核文档入口。本决策不表示所有历史文档都已符合要求，也不表示
任何架构目标已经实现。

## 背景

内核文档需要回答不同读者提出的不同问题：

- 当前检出的软件实际具有什么行为？
- 哪些架构选择会约束后续变更，即使对应迁移尚未完成？
- 一系列变更应当收敛到什么稳定终态？
- 哪个交付切片处于计划中、进行中、已验证或已完成状态？

这些问题具有不同的时间语义。把它们合并在一份持续更新的文档中，会让目标看起来已经实现，
让决策变成进度报告，或让过时迁移阶段继续描述当前行为。

依赖中立内核直接暴露了这种歧义。公共 Host 和 operation 契约已经使用 Photospider 值类型，
而当前私有 Graph、ROI、dirty propagation、planning、cache 和 runtime 代码仍使用 OpenCV
geometry 或 YAML value。维护中的当前状态文档记录该实现；
[ADR 0002](0002-external-libraries-are-kernel-adapters.zh.md) 接受依赖中立约束，但不声称迁移完成；
[内核演进 roadmap](../../roadmap/zh/Kernel-Evolution.zh.md) 描述稳定的迁移后目标。实施状态属于
链接的 GitHub Project 和 Issue。读者不应再依靠措辞猜测这些区别。

## 决策

内核文档采用四个活跃信息层：

| 层级 | 权威来源 | 时间含义 | 允许内容 | 禁止内容 |
| --- | --- | --- | --- | --- |
| 当前事实 | `docs/kernel-architecture/` | 当前源码树中的行为与所有权 | 可观察行为、已实现的所有权与机制、当前限制、不变量、故障语义，以及源码/测试入口 | 未实现设计、迁移阶段、任务状态、勾选项、无日期 TODO，或把未来术语描述为 runtime object |
| 架构决策 | `docs/adr/` | 持久决策及其决策时背景 | 状态、背景、决策、结果、未采用方案、取代关系，以及与当前事实和目标的关系 | 完成百分比、临时任务清单，或在当前事实和证据不匹配时声称目标已经实现 |
| 演进目标 | `docs/roadmap/` | 可以跨越多个交付切片的稳定已接受方向 | 目标所有权、边界、不变量、依赖顺序和必要结果；为解释差异而明确标记的当前基线 | 没有当前状态引用的当前行为、Issue 状态、实施勾选表、排期或完成声明 |
| 实施状态 | 链接的 GitHub Project 和 Issue | 一个交付切片的实时状态与验证结果 | 跟踪键、依赖、验收状态、实际命令与结果、commit 或 PR 证据、风险和后续工作 | 只通过状态字段或勾选项重新定义当前行为、架构决策或目标 |

构建、测试和验证流程继续位于 `docs/development/` 与 `docs/CI/`。已完成迁移、被取代的提案和
阶段报告可以保留在 `docs/outdated/` 或归档 change 记录中，但它们不是第五个活跃事实来源。

### 状态来源与本地工作计划

链接的 GitHub Project item 和 Issue 是 roadmap 切片的公开实施状态来源。活动 OpenSpec change
可以提供 change-local proposal、design、spec delta 与任务清单，但不会成为独立完成权威，也不得
成为理解维护中公开文档所必需的依赖。本地开发看板可以镜像或拆分后续工作，但也不能覆盖
Project 或 Issue 状态。

当前状态文档、ADR 和 roadmap 都不携带实施百分比或勾选项。Project 或 Issue 状态变化本身不会
把目标变成当前事实；只有源码和长期验证支持新行为时，当前文档才随之改变。

### 交叉引用规则

各层通过引用联系，而不是彼此复制：

1. 当前状态文档可以在边界需要决策理由或未来背景时链接对应 ADR 或 roadmap。链接必须把对方
   标为决策或目标，不得把未来对象引入当前术语。
2. ADR 链接促成决策的当前状态证据，以及该决策约束的目标。ADR 背景中的事实属于决策时历史
   事实；维护中的当前行为仍以 `docs/kernel-architecture/` 为准。
3. Roadmap 目标链接其 governing ADR 和迁移起点的当前基线。任何基线摘要都必须明确标记，并
   服从当前状态文档。
4. 交付 Issue 或 PR 引用一组完整证据：相关当前状态文档、governing ADR、精确 roadmap 目标、
   实时 Project/Issue 状态和实际验证证据。

存在翻译目标时，链接应留在同一语言内。英文文档是权威来源；对应 `zh/*.zh.md` 文件在同一变更
中更新为忠实、面向读者的中文翻译。

### 提升流程

把目标变成当前行为是显式提升，而不是措辞修改：

1. 实施前，交付切片确定当前基线、governing decision、目标结果和实时状态 owner。
2. Change-local design 或任务计划可以细化工作，但不会改变其他层的时间含义。
3. 行为落地时，同一变更更新代码、长期测试、相关英文当前状态文档及其中文镜像。
4. 只有已接受目标改变时才修改 roadmap。决策改变时必须新建或以新 ADR 取代原 ADR；实施未变化
   的决策不会重写其历史。
5. 只有具备所需实现和验证证据后，才改变 Project 与 Issue 状态。

### 在依赖解耦交付切片中的应用

下表把本决策应用到
[Project #2](https://github.com/users/kevin-zf1123/projects/2) 与
[父 Issue #51](https://github.com/kevin-zf1123/photospider/issues/51) 的子切片。它固定证据路由与
提升责任，不记录完成状态；每个链接的子 Issue 及其 Project item 仍是实施状态 owner。

| 切片 | 当前事实起点证据 | 决策与目标用法 | 提升责任 |
| --- | --- | --- | --- |
| [#53 / F-2](https://github.com/kevin-zf1123/photospider/issues/53)，重组当前内核文档 | [文档 README](../../kernel-architecture/zh/README.zh.md)、[术语](../../kernel-architecture/zh/Terminology.zh.md)、[数据模型](../../kernel-architecture/zh/Data-Model.zh.md)、[脏区传播](../../kernel-architecture/zh/Dirty-Region-Propagation.zh.md)和[图生命周期](../../kernel-architecture/zh/Graph-Lifecycle.zh.md) | 应用本 ADR；仅把[依赖中立目标](../../roadmap/zh/Kernel-Evolution.zh.md#依赖中立内核)作为显式未来边界 | 重组当前事实，不得把仅属于目标的对象移入当前文档，也不得在当前文档中报告迁移进度。 |
| [#54 / F-3](https://github.com/kevin-zf1123/photospider/issues/54)，kernel geometry 纵向路径 | [脏区传播](../../kernel-architecture/zh/Dirty-Region-Propagation.zh.md)和[数据模型](../../kernel-architecture/zh/Data-Model.zh.md) | 应用 [ADR 0002](0002-external-libraries-are-kernel-adapters.zh.md) 与[依赖中立目标](../../roadmap/zh/Kernel-Evolution.zh.md#依赖中立内核) | 只有纵向路径和长期测试建立事实后，才更新当前 ROI/dirty/planning/execution geometry。 |
| [#55 / F-4](https://github.com/kevin-zf1123/photospider/issues/55)，`ParameterValue` 纵向路径 | [数据模型](../../kernel-architecture/zh/Data-Model.zh.md)和[图生命周期](../../kernel-architecture/zh/Graph-Lifecycle.zh.md) | 应用 [ADR 0002](0002-external-libraries-are-kernel-adapters.zh.md) 与[依赖中立目标](../../roadmap/zh/Kernel-Evolution.zh.md#依赖中立内核) | 只有 document、Graph、operation、error 与 test 证据一致后，才提升 format-neutral parameter 行为。 |
| [#56 / F-5](https://github.com/kevin-zf1123/photospider/issues/56)，从私有接口移除 OpenCV geometry | [术语](../../kernel-architecture/zh/Terminology.zh.md)、[数据模型](../../kernel-architecture/zh/Data-Model.zh.md)和[脏区传播](../../kernel-architecture/zh/Dirty-Region-Propagation.zh.md) | 应用 [ADR 0002](0002-external-libraries-are-kernel-adapters.zh.md) 与[依赖中立目标](../../roadmap/zh/Kernel-Evolution.zh.md#依赖中立内核) | 只有 Graph/ROI/dirty/planning 接口与 regression 使用 kernel geometry 后，才移除当前 OpenCV 限制。 |
| [#57 / F-6](https://github.com/kevin-zf1123/photospider/issues/57)，kernel buffer primitive | [ImageBuffer 内存契约](../../kernel-architecture/zh/ImageBuffer-Memory-Contract.zh.md)、[数据模型](../../kernel-architecture/zh/Data-Model.zh.md)和[脏区传播](../../kernel-architecture/zh/Dirty-Region-Propagation.zh.md) | 应用 [ADR 0002](0002-external-libraries-are-kernel-adapters.zh.md) 与[依赖中立目标](../../roadmap/zh/Kernel-Evolution.zh.md#依赖中立内核) | 只有 padded-row 产品路径测试通过后，才记录当前 stride-aware tiled normalization 与 metrics。 |
| [#58 / F-7](https://github.com/kevin-zf1123/photospider/issues/58)，可选 OpenCV operation provider | [概览](../../kernel-architecture/zh/Overview.zh.md)和[插件 ABI](../../kernel-architecture/zh/Plugin-ABI.zh.md) | 应用 [ADR 0002](0002-external-libraries-are-kernel-adapters.zh.md)，保留 [ADR 0004](0004-opencv-cpu-operations-are-reentrant-provider-work.zh.md)，并使用[依赖中立目标](../../roadmap/zh/Kernel-Evolution.zh.md#依赖中立内核) | 只有 initialization、exception、algorithm 与 replacement 证据都处于 provider 边界后方时，才更新当前模块/provider 所有权。 |
| [#59 / F-8](https://github.com/kevin-zf1123/photospider/issues/59)，注入 image/artifact codec | [缓存模型](../../kernel-architecture/zh/Cache-Model.zh.md)和[图生命周期](../../kernel-architecture/zh/Graph-Lifecycle.zh.md) | 应用 [ADR 0002](0002-external-libraries-are-kernel-adapters.zh.md)，保留 [ADR 0005](0005-graph-document-ingestion-is-a-classified-transaction.zh.md)，并使用[依赖中立目标](../../roadmap/zh/Kernel-Evolution.zh.md#依赖中立内核) | 只有 cache lifecycle 与 error test 不再依赖直接 OpenCV codec call 后，才提升 codec injection。 |
| [#60 / F-9](https://github.com/kevin-zf1123/photospider/issues/60)，`GraphDefinition` 与 in-memory document adapter | [数据模型](../../kernel-architecture/zh/Data-Model.zh.md)和[图生命周期](../../kernel-architecture/zh/Graph-Lifecycle.zh.md) | 应用 [ADR 0002](0002-external-libraries-are-kernel-adapters.zh.md)，保留 [ADR 0005](0005-graph-document-ingestion-is-a-classified-transaction.zh.md)，并使用[依赖中立目标](../../roadmap/zh/Kernel-Evolution.zh.md#依赖中立内核) | 只有 load/reload/save 在不使用临时 YAML 的情况下保留已分类 transaction 后，才提升 format-neutral document path。 |
| [#61 / F-10](https://github.com/kevin-zf1123/photospider/issues/61)，注入 YAML filesystem adapter | [概览](../../kernel-architecture/zh/Overview.zh.md)和[图生命周期](../../kernel-architecture/zh/Graph-Lifecycle.zh.md) | 应用 [ADR 0002](0002-external-libraries-are-kernel-adapters.zh.md)，保留 [ADR 0005](0005-graph-document-ingestion-is-a-classified-transaction.zh.md)，并使用[依赖中立目标](../../roadmap/zh/Kernel-Evolution.zh.md#依赖中立内核) | 只有 composition-root injection 与 Host format-neutral 行为通过验证后，才把 YAML 描述为当前 adapter。 |
| [#62 / F-11](https://github.com/kevin-zf1123/photospider/issues/62)，移除 YAML runtime/cache value | [数据模型](../../kernel-architecture/zh/Data-Model.zh.md)和[缓存模型](../../kernel-architecture/zh/Cache-Model.zh.md) | 应用 [ADR 0002](0002-external-libraries-are-kernel-adapters.zh.md) 与[依赖中立目标](../../roadmap/zh/Kernel-Evolution.zh.md#依赖中立内核) | 只有 Node、Graph、compute、inspection 与 cache 接口及 regression 使用 format-neutral value 后，才移除当前 YAML runtime/cache 表述。 |
| [#63 / F-12](https://github.com/kevin-zf1123/photospider/issues/63)，dependency-disabled build profile | [概览](../../kernel-architecture/zh/Overview.zh.md)和[测试与验证](../../development/zh/Testing-and-Validation.zh.md) | 把 [ADR 0002](0002-external-libraries-are-kernel-adapters.zh.md)作为验收约束，把[依赖中立目标](../../roadmap/zh/Kernel-Evolution.zh.md#依赖中立内核)作为必要结果 | 在 Issue 中记录实际 build/install-consumer 证据；只有 dependency-disabled 产品路径通过后，才把该 profile 描述为当前事实。 |

## 结果

- 当前架构文档可以直接作为实现证据，无需先过滤未来计划。
- ADR 可以在迁移完成前接受目标约束，同时明确表达该状态。
- Roadmap 跨单个 commit 保持稳定，不会成为重复项目看板。
- 依赖解耦交付切片可以使用一套明确证据，不再推测移除 OpenCV/YAML 到底是当前行为、已接受
  约束、目标还是已完成工作。
- 重组当前状态文档并不授权把仅属于目标的概念移入当前 glossary；反过来，完成一个纵向切片
  必须更新相关当前事实，而不能只勾选 Issue。
- 即使个人或 change-local 工作流材料不存在，维护中的源码文档在干净主仓库中仍可独立理解。

## 未采用的方案

### 把当前行为与未来方向保留在一份持续更新的架构文档中

不采用，因为如果没有遍布全文且脆弱的时间标签，同一段落无法同时成为稳定目标背景和权威当前
行为。

### 用 ADR 状态表示实施状态

不采用，因为接受决策与完成迁移是两个事件。ADR 0002 和 ADR 0003 有意保持为已接受的目标约束，
而当前所有权记录在其他位置。

### 把 Project 勾选表复制进 roadmap

不采用，因为重复的实时状态会漂移，并把持久目标变成阶段报告；roadmap 应链接状态 owner。

### 把已完成 Issue 当成当前行为的充分证据

不采用，因为状态元数据无法验证源码、测试、公共契约或文档。提升要求这些产物彼此一致。

## 决策证据

本决策的首次应用基于以下文档完成审查：

- [内核术语](../../kernel-architecture/zh/Terminology.zh.md)；
- [内核数据模型](../../kernel-architecture/zh/Data-Model.zh.md)；
- [脏区传播与工作选择](../../kernel-architecture/zh/Dirty-Region-Propagation.zh.md)；
- [图生命周期与变更语义](../../kernel-architecture/zh/Graph-Lifecycle.zh.md)；
- [ADR 0002](0002-external-libraries-are-kernel-adapters.zh.md)；以及
- [内核演进目标](../../roadmap/zh/Kernel-Evolution.zh.md)。
