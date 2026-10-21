# ADR 0014：Compiler 阶段与兼容版本保持独立

- 状态：已接受

## 当前实现

Package 0.39.0 的所有合法 IR 版本统一使用 `ValidatedIR`。Analysis、可选命名 optimization 与命名 physical planning 保持分离。Call ID 顺序定义 retirement/effect 顺序，task dependency 定义 issue 顺序。Planner 生成不可变物理 Task/Tile/Call/Block 表，显式 CFG loop 保留 activation 语义。参见[版本契约](../../development/zh/Compiler-Version-Contract.zh.md)。

`.psir` 与 `.psxp` 使用 format 5。旧 artifact 需要重新编译/导出，不提供 continuation Plan 转换。Import 校验保存的表并重新绑定静态契约/resource，不执行 optimizer、lowerer 或 planner。Device preparation 是后续显式阶段。

## 1. 核心摘要

Document schema、operation declaration、plugin ABI、package API、artifact encoding 与 compiler identity 描述不同契约。一个轴改变不暗示其他轴兼容或必须变更版本。Optimization 产生另一个不可变 `ValidatedIR`，不要求不同的公共阶段类型。

## 2. 阶段与 identity

Analysis 产生经过校验的源语义。可选命名 optimizer 保持必需语义顺序，返回独立校验的不可变版本。Planner 接受合法版本，记录实际输入 digest、execution mode、物理表和选定 return。Planner 名称与版本用于标识编译诊断；planner 选择属于编译选项。Plan 保存生成的物理契约，planner provenance 不进入保存内容。

IR content identity 不含 parent version 与变换历史等 provenance。Plan identity 包含物理执行契约；即使所有已保存 task 均为 CPU，将 `CpuExact` 改为 `NativeGpu` 也会改变 identity。Preparation 绑定具体 device 与 kernel，不改变 Plan 拓扑或 identity。

## 3. 兼容版本轴

| 版本轴 | 当前契约 |
| --- | --- |
| WorkflowDocument schema | 7 |
| Result schema encoding | 6 |
| OperationTraits | 30 |
| Operation module / fixed invocation C ABI | 8 / 8 |
| Native kernel service | 2 |
| IR / Plan artifact format | 5 / 5 |
| Package / SOVERSION | 0.39.0 / 0.39 |

Compiler 在相应 content domain 编码封闭文档字段、规范化参数、复制的 operation 契约、静态特化与声明的 resource identity。Physical identity 还包括 schema、layout、tile、placement、task、邻接表、call membership、CFG、selection 与 reservation bound。

每个进程内阶段保留 exact frozen operation registry 的私有 weak identity。Optimizer、planner 和 execution preparation 拒绝 foreign-registry stage，即使 key 相同。该运行时 identity，以及 native address、allocation ID、timing、cancellation、queue state、daemon ID 均不进入规范化 digest 或序列化数据。

Float 参数 identity 按固定小端顺序保留复制得到的 binary64 位，包括 signed zero 与 schema 允许的非有限 payload。Callback address 与 opaque static-state pointer 不编码；声明的不可变契约/resource identity 按对应契约参与身份。

Plan 是经过校验的进程内值与版本化 compiler artifact，不是 daemon wire message。Content digest 标识内容，不认证 native code。Source-bound Plan 保留 graph-currentness 检查；独立导入的 Plan 与源图无关。保存 Plan 不保存 Run state、payload cache 或 device handle。

## 4. 后果

Consumer 分别维护自己公开的边界版本，并在源语义、operation 契约、planner 行为或选定 placement 改变时重新编译派生 Plan。保存的 artifact 在 preparation 前必须通过有界解析、结构校验与契约链接。Workflow schema 不变时，package 仍可发生不兼容变化。本决策不提供跨版本 ABI 或 artifact 兼容保证。
