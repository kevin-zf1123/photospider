# 当前开发计划

- 快照日期：2026-09-09
- 已审计基础：main@0e65eac（已结算 S3，PR #278）
- 当前 milestone：S4 Metal 执行与驻留图像 workflow

## 角色与权威

本文件记录公开 delivery baseline、当前 milestone、active leaf Issue、dependency 与
执行顺序。它不能修改 ADR 0015 的产品边界，也不能修改
`docs/kernel-architecture/` 记录的行为。

公开 GitHub Issue 是 live delivery-status authority。本快照与 Issue 不一致时，以 Issue
为准，并同步修订本文件。GitHub Project 是 maintainer operational view，只同步 Issue
状态，不能覆盖 Issue。私有 personal-overlay OpenSpec 文件属于 maintainer working
note，没有公开 authority，也不构成交付 gate。

Active Project container 是
[#7 FND](https://github.com/users/kevin-zf1123/projects/7)、
[#8 IR](https://github.com/users/kevin-zf1123/projects/8)、
[#9 HEX](https://github.com/users/kevin-zf1123/projects/9) 与
[#10 MED](https://github.com/users/kevin-zf1123/projects/10)。S1 kernel Issue 改变
compiler 与 execution contract，因此进入 Project #8。

## 已结算实现 baseline

以下能力在已审计 baseline 已经完整交付，并已与对应 GitHub Issue 对齐：

| 领域 | 已交付 Issue | 当前证据 |
| --- | --- | --- |
| 紧凑 build 与 validation profile | [#192](https://github.com/kevin-zf1123/photospider/issues/192)、[#193](https://github.com/kevin-zf1123/photospider/issues/193) | `CMakeLists.txt`、`CMakePresets.json`、`.github/workflows/ci.yml` 与测试文档 |
| Package 与 public-version 边界 | [#196](https://github.com/kevin-zf1123/photospider/issues/196)、[#198](https://github.com/kevin-zf1123/photospider/issues/198) | `docs/development/Compiler-Version-Contract.md`、package export 与隔离 installed consumer |
| Typed source 与 compiler stage | [#199](https://github.com/kevin-zf1123/photospider/issues/199)、[#200](https://github.com/kevin-zf1123/photospider/issues/200)、[#201](https://github.com/kevin-zf1123/photospider/issues/201)、[#202](https://github.com/kevin-zf1123/photospider/issues/202) | 公开 WorkflowDocument、operation trait、semantic/optimized IR、physical plan、typed digest 与 focused test |
| Raw benchmark vertical | [#240](https://github.com/kevin-zf1123/photospider/issues/240) | `RawBenchmarkRunner`、named oracle 或显式 unchecked 状态、raw diagnostic 与 execution regression |

历史 S0 baseline CI 是
[`kernel-ci` run 68](https://github.com/kevin-zf1123/photospider/actions/runs/33738054894)。
它在 Linux 与 macOS 上通过 static/shared kernel、ASAN 与 TSAN。

## 当前 milestone

S3 已由 [kernel PR #278](https://github.com/kevin-zf1123/photospider/pull/278)
在 main@0e65eac 结算。区域缓存与冻结执行是 S4 基础。

[S4 #279](https://github.com/kevin-zf1123/photospider/issues/279) 实现
[ADR 0019](../../adr/0019-metal-resident-image-workflows.md)：package 0.6、
operation ABI/traits 6、显式 CpuExact/MetalFp32 规划、原生 shared storage 与同步完成、
八个可复用算子、区域上传、有界跨 Run 驻留、回退及诊断。C++17、schema 2 和
provider ABI 1 保持。

叶项按 #280、#281、#153、#154、#282、#283、#156、#284 顺序推进；daemon #19
消费安装包并保持 IPC v3。[S4 workflow 指南](../../kernel-architecture/zh/S4-Workflow.zh.md)
提供公开示例和独立 oracle。Issue 记录验证、独立审查、CI/bot 修复与合并状态；
本地实现不表示远端已结算。成本校准和自动选址 #209 保留到 S5；增量编译 #203、
其他 GPU 后端、GUI 与 IPC 扩展不属于 S4。

## 当前 milestone 以外的 active backlog

- [#246](https://github.com/kevin-zf1123/photospider/issues/246) 只保留可复用 operation
  starter、external consumer example 与精简 usage guide；现有正负 DSO fixture 是
  baseline。
- [#247](https://github.com/kevin-zf1123/photospider/issues/247) 只保留可复用
  data-provider starter、external consumer example 与精简 usage guide；现有 provider
  ABI fixture 是 baseline。
- [#248](https://github.com/kevin-zf1123/photospider/issues/248) 保留可选、由 embedding
  拥有的 operation-set manifest。它对 WorkflowDocument 定义的既有 dependency 已经
  完成，因此 Issue 解除 blocked 状态，等待优先级判断。
- [#148](https://github.com/kevin-zf1123/photospider/issues/148) 只保留 structured
  explain 与剩余显式 IR/plan validator delta。
- [#149](https://github.com/kevin-zf1123/photospider/issues/149) 与
  [#203](https://github.com/kevin-zf1123/photospider/issues/203) 在 S1 后依次处理
  trait-proven optimization 与 disposable incremental recompilation。
- [#151](https://github.com/kevin-zf1123/photospider/issues/151) 和 #152 保留 S5
  成本与校准工作；S4 显式选址不依赖 #209，也不结算这些上层 Issue。
- MED work 只由选定 operation vertical 的 semantic 需求启用。

## Issue 执行契约

可执行 leaf Issue 记录 audited baseline commit、remaining delta、governing public
document、public/API/schema impact、start dependency、integration dependency、
completion gate、named fixture 或 vertical、精确 test 与 oracle、non-goal，以及预期
completion evidence。Parent Issue 只作为 index 与 closure aggregator，不携带
`ready-for-agent`。

任务状态、授权终点和决策/实现完成条件见[任务协作](Task-Collaboration.zh.md)。

## 更新规则

Audited baseline、当前 milestone、critical path 或 blocked reason 变化时更新本快照。
普通 implementation detail 保留在所属 Issue 与 test 中。每项 status claim 必须引用已
完成 code 与 test；unchecked item 不定义当前行为。

## 已接受的开发方向，2026-09-05

维护者已明确回复“接受建议内容，采用调整后的方向。”，采用
[开发计划](Refactor-Development-Plan.zh.md) 的 S1 图像与普通参数、S2 CPU 区域、
S3 缓存与交互、S4 原生 GPU、S5 实测优化方向。Float32 是 S1 已接受目标，
daemon 新功能按需推进，兼容维护继续。决策交付状态由
[#256](https://github.com/kevin-zf1123/photospider/issues/256) 跟踪。

ADR 0016 保留 S1 source/binding/profile 契约。ADR 0017 替代其整图存储/输出、整图
扫描和估算预算条款。Daemon 兼容维护消费安装的 0.6；新 bindings 和 bulk transport
继续由 daemon 仓库按实际需求推进。
