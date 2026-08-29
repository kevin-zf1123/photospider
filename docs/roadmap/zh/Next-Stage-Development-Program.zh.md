# 下一阶段开发计划

## 状态与权威性

本文是 GitHub Projects #1 至 #6 之后开发计划的权威 Portfolio Architecture v1。
[下一阶段执行计划](Next-Stage-Execution-Plan.zh.md) 是独立的 Issue 级 dependency、readiness、
descendant 与 delivery-wave 权威图。两者都记录预期工作，而不是当前软件行为。当前行为仍以
`docs/kernel-architecture/` 为权威，架构决策仍以 `docs/adr/` 为权威，
实时交付状态则以所链接的 GitHub Projects 和 Issues 为权威。

该项目组合由八个私有、用户级 GitHub Projects 组成。每个 Project 在
`kevin-zf1123/photospider` 中有一个 open 父 Issue、direct program slices，以及 Execution Plan
v2 所需有界 native descendants；direct-child 与 Project-item 数量均不固定。关闭规划
任务、修改 Project 字段或完成探索原型，都不会把目标提升为当前行为。完成提升需要具备明确
范围的架构与 OpenSpec 权威、实现、长期测试、同步的中英文文档，以及下文规定的完成证据。

## 起始基线

已关闭的 Projects #1 至 #6 建立了本计划要延伸的边界：Graph identity 与 revision、
request-owned `ComputeRun`、process-owned `ExecutionService`、取消与 latest-wins 仲裁、资源核算、
通用 `Value`/`Region`/`Binding`/`ReadyFence` 契约、纯 C operation plugin ABI、隔离 worker，以及
单租户持久作业纵向切片。新 Projects 必须复用这些权威，不能引入平行的 graph、scheduling、
resource、output、plugin 或 durable-job owner。

核心缺失层是这条显式管线：

```text
WorkflowDocument
        ↓
SemanticGraphIR
        ↓
OptimizedGraphIR
        ↓
ExecutionPlan
        ↓
现有 Run 与执行域
```

剩余产品工作围绕该管线切分，使工程风险、编译器语义、执行策略、媒体含义与面向产品的纵向
切片可以推进，而无需依赖一个范围无限扩张的总括 Project。

## 项目组合

| 顺序 | Project | 父 Issue | 优先级 | 领域 | 依赖 | 预期结果 |
| --- | --- | --- | --- | --- | --- | --- |
| #7 | [engineering-foundations-plugin-dx](https://github.com/users/kevin-zf1123/projects/7) | [#139](https://github.com/kevin-zf1123/photospider/issues/139) | P0 | Engineering | — | 可维护的构建、包、源码、安全、持久化、发布与插件开发基础。 |
| #8 | [graph-ir-optimization-planning](https://github.com/users/kevin-zf1123/projects/8) | [#145](https://github.com/kevin-zf1123/photospider/issues/145) | P0 | Compiler | — | 显式、不可变、可解释的 document-to-plan 编译管线，以及保持语义的图优化。 |
| #9 | [cost-aware-heterogeneous-execution](https://github.com/users/kevin-zf1123/projects/9) | [#151](https://github.com/kevin-zf1123/photospider/issues/151) | P1 | Execution | #8 | 统一 ROI、tile、cache、memory、transfer 与 device 规划，并安全执行 device-resident 链。 |
| #10 | [media-semantics-color-time](https://github.com/users/kevin-zf1123/projects/10) | [#157](https://github.com/kevin-zf1123/photospider/issues/157) | P1 | Media | #8 | 所有消费者共享的显式 color、alpha、channel、frame、time 与 sample 含义。 |
| #11 | [interactive-viewer-editing](https://github.com/users/kevin-zf1123/projects/11) | [#163](https://github.com/kevin-zf1123/photospider/issues/163) | P2 | Interactive | #8、#9、#10 | Viewer session、可撤销编辑、stroke、dirty tile、取消与 progressive presentation。 |
| #12 | [progressive-renderer-outputs](https://github.com/users/kevin-zf1123/projects/12) | [#169](https://github.com/kevin-zf1123/photospider/issues/169) | P2 | Renderer | #8、#9、#10 | Renderer adapter、progressive tile、AOV、分阶段 deep/multiview output 与 checkpoint validation。 |
| #13 | [python-testbench-batch-automation](https://github.com/users/kevin-zf1123/projects/13) | [#175](https://github.com/kevin-zf1123/photospider/issues/175) | P2 | Automation | #8、#10 | 稳定 Python facade、算法 testbench、sequence orchestration、provenance、resume 与 packaging。 |
| #14 | [multi-tenant-production-services](https://github.com/users/kevin-zf1123/projects/14) | [#181](https://github.com/kevin-zf1123/photospider/issues/181) | P2 | Service | #7、#9、#13 | 经过认证、quota 治理、隔离、持久且可观测的生产服务边界。 |

## 依赖与交付顺序

```text
                    ┌──────────────────────────────────┐
                    │ #7 工程基础                      │
                    └────────────────┬─────────────────┘
                                     │
                                     └──────────────────────────┐

┌──────────────────────────────────┐                            │
│ #8 graph IR 与 planning         │                            │
└───────────────┬──────────────────┘                            │
                ├───────────────┐                               │
                ↓               ↓                               │
┌───────────────────────┐  ┌────────────────────────┐           │
│ #9 异构执行            │  │ #10 媒体语义           │           │
└───────────┬───────────┘  └───────────┬────────────┘           │
            │                          │                        │
            ├──────────────┬───────────┼─────────────┐          │
            ↓              ↓           ↓             ↓          │
     ┌────────────┐  ┌────────────┐  ┌────────────┐             │
     │ #11 viewer │  │ #12 render │  │ #13 Python │─────────────┤
     └────────────┘  └────────────┘  └────────────┘             │
                                                                  ↓
                                                        ┌────────────────┐
                                                        │ #14 services   │
                                                        └────────────────┘
```

Projects #7 与 #8 并行开始。Project #8 建立 #9 与 #10 所需的类型化 planning seam。
Projects #11 与 #12 同时依赖执行和媒体语义；#13 依赖 graph IR 与 temporal/media 语义。
Project #14 被有意安排在最后：在考虑不受信任的多租户暴露前，它需要工程加固、异构资源策略与
持久批处理自动化。

Portfolio table 中的“依赖”表示 product-completion relationship，而不是无差别 start gate。每个
implementation Issue 在 Execution Plan v2 中分别记录 Start、Integration、Completion。满足精确
Start dependencies 时，可以提前开展有界研究、接口探索、contract 与 fixture 准备。

## 通用 Project 模型

八个 Projects 全部使用 `Portfolio` table view 和相同的规划字段：

| 字段 | 取值或作用 |
| --- | --- |
| `Status` | `Todo`、`In Progress`、`Done` |
| `Priority` | `P0`、`P1`、`P2`、`P3` |
| `Area` | `Engineering`、`Compiler`、`Execution`、`Media`、`Interactive`、`Renderer`、`Automation`、`Service` |
| `Phase` | `Discovery`、`Contract`、`Vertical Slice`、`Integration`、`Hardening`、`Production` |
| `Target` | `Foundation`、`Vertical Slice`、`Product`、`Production` |
| `Risk` | `Low`、`Medium`、`High` |
| `Work Type` | `AFK`、`HITL` |
| `Verification` | `Planned`、`In Review`、`Verified`、`Blocked` |

GitHub 保留 `Type` 并拒绝把它作为 Project V2 自定义字段名，因此既有 AFK/HITL 分流概念用
`Work Type` 表示。

仓库目前没有 release milestone，本项目组合也不会在缺乏发布证据时发明日历承诺。Phase、
target 与 dependency fields 承载当前排序意图；未来 milestone 必须表示另行批准的发布边界。

每个 Project parent 保留 direct program slices。Epic-sized slice 是带有有界 native descendants
的 aggregate Issue；Project completion 要求 verified descendant closure，而不是固定 direct-child
count。Parent 使用 `ready-for-human`；contract Issue 使用 `ready-for-human`，不完整 aggregate/
implementation work 使用 `needs-triage`。`Work Type` 不表示 readiness，只有满足公共 Definition
of Done promotion checklist 后才能使用 `ready-for-agent`。标准 triage labels 继续作为权威；本
计划不创建竞争角色 labels。

每个 Issue 使用[执行切片完成定义](../../development/zh/Execution-Slice-Definition-of-Done.zh.md)中的
可执行结构，链接精确 upstream slices，并分别记录 Start、Integration、Completion。Parent 与
aggregate Issue 维护真实 native descendant checklist。所有未来开发 Issues 在自身证据完整前
保持 open。

## Project #7：工程基础与插件 DX

目标：在增加新产品领域时，让内核继续易于修改、打包、保护、诊断和扩展。

不变量：

- runtime 行为与 public/plugin 契约仅通过有明确范围且经过测试的 proposal 修改；
- build、package 与 CI 门禁验证长期软件行为，而不是迁移残留；
- 复杂度削减保留已建立的所有权与生命周期边界，不留下兼容 wrapper。

非目标包括重新实现 Projects #1–#6 已交付的能力，以及把源码质量或迁移残留审计注册为产品
CTest/CI 测试。

| Issue | 可执行切片 |
| --- | --- |
| [#140](https://github.com/kevin-zf1123/photospider/issues/140) | 加固 CI、CMake、packaging 与 security gates。 |
| [#141](https://github.com/kevin-zf1123/photospider/issues/141) | 拆分 Host mega-interface 并澄清 facade 所有权。 |
| [#142](https://github.com/kevin-zf1123/photospider/issues/142) | 降低 ready-store、worker-loop 与 monitor 复杂度。 |
| [#143](https://github.com/kevin-zf1123/photospider/issues/143) | 交付 plugin SDK tooling、templates 与 conformance harnesses。 |
| [#144](https://github.com/kevin-zf1123/photospider/issues/144) | 建立跨平台持久化与发布治理。 |

## Project #8：Graph IR、优化与 Planning

目标：在用户工作流意图与现有执行内核之间创建缺失的编译器/planning 层。

文档、语义意图、优化后拓扑和物理执行计划保持为彼此不同的不可变 artifact。优化保留声明的
media 与 value 语义，生成确定性 provenance，而且绝不把 topology authority 移入 scheduling。
在这些契约得到证明前，不承诺 backend-specific code generation。

| Issue | 可执行切片 |
| --- | --- |
| [#146](https://github.com/kevin-zf1123/photospider/issues/146) | 定义 `WorkflowDocument` 与 `SemanticGraphIR` 契约。 |
| [#147](https://github.com/kevin-zf1123/photospider/issues/147) | 把 `SemanticGraphIR` lowering 为 `OptimizedGraphIR` 与 `ExecutionPlan`。 |
| [#148](https://github.com/kevin-zf1123/photospider/issues/148) | 实现 validation 与 explain/inspection tooling。 |
| [#149](https://github.com/kevin-zf1123/photospider/issues/149) | 实现确定性的 dead、identity、constant 与 common-subexpression passes。 |
| [#150](https://github.com/kevin-zf1123/photospider/issues/150) | 实现 transform concatenation、pointwise fusion、channel pruning 与 static-subgraph hoisting。 |

## Project #9：成本感知的异构执行

目标：把当前 device、residency、transfer 与 resource primitives 变成成本感知的端到端执行
planner。

Host resource ledger 与精确所有权 identity 继续作为权威。Transfer 与 residency 是带可观测
fallback 的显式计划决策；不允许隐式隐藏 copy。在形成代表性基线前，不接受通用 GPU 加速承诺或
数值性能目标。

| Issue | 可执行切片 |
| --- | --- |
| [#152](https://github.com/kevin-zf1123/photospider/issues/152) | 定义统一的 ROI、tile、cache、memory 与 device 成本模型。 |
| [#153](https://github.com/kevin-zf1123/photospider/issues/153) | 规划 operation chains 之间的显式 transfer 与 device residency。 |
| [#154](https://github.com/kevin-zf1123/photospider/issues/154) | 以安全 fallback 执行 GPU-resident operation chains。 |
| [#155](https://github.com/kevin-zf1123/photospider/issues/155) | 以可复现 benchmark 校准 memory、cache 与 tile 预算。 |
| [#156](https://github.com/kevin-zf1123/photospider/issues/156) | 增加 heterogeneous-plan 可解释性与回归门禁。 |

## Project #10：媒体语义、色彩、通道与时间

目标：让媒体含义足够显式，以支持正确的优化、合成、渲染和序列处理。

存储表示、声明的媒体语义与观测统计保持不同。Color、alpha、channel 与 time transformation
必须显式且带版本。存在歧义的转换与优化 fail closed。内核不嵌入某个 studio configuration，也
不从文件名推断权威含义。

| Issue | 可执行切片 |
| --- | --- |
| [#158](https://github.com/kevin-zf1123/photospider/issues/158) | 定义 color-space 与 OCIO 集成边界。 |
| [#159](https://github.com/kevin-zf1123/photospider/issues/159) | 定义 alpha association 与 compositing 语义。 |
| [#160](https://github.com/kevin-zf1123/photospider/issues/160) | 定义 channel sets、names、routing 与 pruning。 |
| [#161](https://github.com/kevin-zf1123/photospider/issues/161) | 定义 frame、time、sample 与 sequence 语义。 |
| [#162](https://github.com/kevin-zf1123/photospider/issues/162) | 建立媒体语义参考 corpus 与 conformance suite。 |

## Project #11：交互式 Viewer 与编辑

目标：交付低延迟编辑循环，同时不把 graph、resource 或 commit authority 移入 UI。

Viewer state 永不成为 graph 或 execution authority。编辑、历史、stroke、dirty tile、取消与
presentation 具有显式 identity 和 commit boundary。内核契约不选择永久 GUI toolkit，延迟目标
遵循既有“先基线、后目标”协议。

| Issue | 可执行切片 |
| --- | --- |
| [#164](https://github.com/kevin-zf1123/photospider/issues/164) | 定义 viewer session、render request 与 presentation 契约。 |
| [#165](https://github.com/kevin-zf1123/photospider/issues/165) | 实现 undo 与 redo 编辑 transaction。 |
| [#166](https://github.com/kevin-zf1123/photospider/issues/166) | 以 dirty tile 实现 brush 与 stroke 生命周期。 |
| [#167](https://github.com/kevin-zf1123/photospider/issues/167) | 实现低延迟更新、取消与 quality ladder。 |
| [#168](https://github.com/kevin-zf1123/photospider/issues/168) | 建立交互 benchmark 与确定性 UI integration harness。 |

## Project #12：渐进式 Renderer 与输出

目标：让 Photospider 成为面向 renderer 的 graph 与 execution kernel，同时不把 renderer 所有权
嵌入 core。

Renderer backend 保持为版本化契约之后的 adapter/provider。Progressive update 保留 revision、
channel、time、ownership 与 completion identity。Deep 与 multiview 支持分阶段且显式限定；
首个切片不声称覆盖全部 deep tiled 或 multipart 能力。

| Issue | 可执行切片 |
| --- | --- |
| [#170](https://github.com/kevin-zf1123/photospider/issues/170) | 定义 renderer backend 与 execution 契约。 |
| [#171](https://github.com/kevin-zf1123/photospider/issues/171) | 实现 progressive tile scheduling 与 convergence。 |
| [#172](https://github.com/kevin-zf1123/photospider/issues/172) | 定义并传输 AOV 与 channel output。 |
| [#173](https://github.com/kevin-zf1123/photospider/issues/173) | 增加分阶段的 deep 与 multiview output 支持。 |
| [#174](https://github.com/kevin-zf1123/photospider/issues/174) | 建立 renderer corpus、golden validation 与 checkpoint resume。 |

## Project #13：Python Testbench 与批处理自动化

目标：让内核可用于图像算法开发与高吞吐自动化，同时不绕过 Host、job 或 artifact 契约。

Python 暴露 owned value 与稳定的 error/lifetime 语义，绝不暴露 borrowed mutable kernel state。
Batch manifest、provenance、outcome 与 resume decision 是持久且确定性的。便利 API 不成为第二个
graph 或 execution authority，首个切片也不把 CPython 嵌入内核。

| Issue | 可执行切片 |
| --- | --- |
| [#176](https://github.com/kevin-zf1123/photospider/issues/176) | 定义稳定 Python facade 与 ownership model。 |
| [#177](https://github.com/kevin-zf1123/photospider/issues/177) | 用独立 oracle 建立图像算法 testbench。 |
| [#178](https://github.com/kevin-zf1123/photospider/issues/178) | 实现 batch sequence 与 frame orchestration。 |
| [#179](https://github.com/kevin-zf1123/photospider/issues/179) | 增加高吞吐 manifest、provenance 与 resume。 |
| [#180](https://github.com/kevin-zf1123/photospider/issues/180) | 打包 Python SDK、examples 与跨平台 CI。 |

## Project #14：多租户生产服务

目标：交付网络化多租户服务边界，同时不把当前本地 sidecar 重新命名为 public server。

Tenant identity、authorization、quota、durable state、worker isolation 与 artifact access 是显式
安全域。单租户 job 纵向切片保持有效，直到通过有明确范围的迁移替换每项权威。当前本地 IPC
sidecar 绝不直接暴露给不受信任网络；生产 SLO 必须先有可复现的负载与故障基线。

| Issue | 可执行切片 |
| --- | --- |
| [#182](https://github.com/kevin-zf1123/photospider/issues/182) | 定义 tenant identity、authentication、authorization 与 quota 契约。 |
| [#183](https://github.com/kevin-zf1123/photospider/issues/183) | 把 durable job state 扩展至 tenant namespace。 |
| [#184](https://github.com/kevin-zf1123/photospider/issues/184) | 建立 production API 与幂等 control plane。 |
| [#185](https://github.com/kevin-zf1123/photospider/issues/185) | 加固 worker pool、artifact plane 与 network isolation。 |
| [#186](https://github.com/kevin-zf1123/photospider/issues/186) | 建立 service baseline、load/chaos test 与运营就绪度。 |

## 完成门禁

可执行子 Issue 仅在以下各项均满足后才算完成：

1. 已记录其当前状态基线与精确边界。
2. 在实现修改权威或 public contract 前，具有明确范围的 OpenSpec change 与任何必要 ADR 已获批准。
3. 实现与长期行为测试按需覆盖成功、失败、并发、取消、资源结算及 package/consumer 边界。
4. 英文权威文档与忠实中文镜像已经同步。
5. 验证记录列出精确命令、环境、原始证据与限制；数值目标仅在代表性基线评审后采用。
6. 子 Issue、原生父子关系、Project 字段与依赖链接和已交付范围一致。

一个 Project 仅在全部 required direct slices 与 native descendants 满足上述门禁、父级证据通过
评审、下游依赖已经更新、不再有重复 authority 或永久兼容层，并且 Project 被显式关闭后才算完成。研究结果可以修改或否决规划
设计；它们不能被静默转换为产品声明。

## 规划 Change 边界

Active OpenSpec change `revise-next-stage-execution-plan` 管理 v2 planning revision、有界 hierarchy、
Project fields、flagship 与这些规划文档。它不实现任何 future development Issue。每个修改软件
行为的 slice 都需要自己的有界 proposal（或明确兼容的小改动）、tests 与 completion evidence。
Planning change 在 human acceptance 前保持 active。
