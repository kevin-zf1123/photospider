# 当前开发计划

- 快照日期：2026-09-09
- 已审计实现 baseline：`d85e7b8`（S2），前序 main 为 `70b760f`
- 当前 milestone：S2 CPU 区域执行与 daemon 安装消费

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

S1 已在 main@70b760f 结算。S2 按已接受的
[ADR 0017](../../adr/zh/0017-cpu-regional-execution-and-storage.zh.md)
实现 kernel 0.4.0、OperationTraits 4 和 operation ABI 4，保留 C++17、schema 2 和
provider ABI 1。按顺序的本地提交如下：

| Issue | 实现 | Commit |
| --- | --- | --- |
| [#263](https://github.com/kevin-zf1123/photospider/issues/263) | 接受研究与存储/执行契约 | f0c1ae0 |
| [#264](https://github.com/kevin-zf1123/photospider/issues/264) | 区域 Value、宿主缓冲区、ABI4 和安装消费 | cd9bdcc |
| [#210](https://github.com/kevin-zf1123/photospider/issues/210) | 按完成释放及受限 workspace | b3cbc52 |
| [#211](https://github.com/kevin-zf1123/photospider/issues/211) | 惰性 tile 与静态 halo | 40dfd69 |
| [#265](https://github.com/kevin-zf1123/photospider/issues/265) | 区域源、结果收集和有序流式执行 | d9f4032 |
| [#266](https://github.com/kevin-zf1123/photospider/issues/266) | Gaussian/曝光/蒙版/source-over 公开场景 | 3b08b41 |
| [daemon #15](https://github.com/kevin-zf1123/photospider-daemon/issues/15) | 消费安装的 kernel 0.4，保留 IPC 子集 | 53ec2ca（daemon） |

独立全面审查修复位于 d85e7b8，固定 fuzz seed 迁移修复位于 921ad5c。直接调用共享受检查
的需求推导，Whole 链及时释放祖先，C 图像地址/clamp 有独立回归，计算产生的蒙版错误
保持 OperationFailed 分类。本地复核未发现剩余 blocker/required。

S2Image.RegionAndTiles 通过 C++ 和受维护 C module 的公开入口运行，整图/分块逐位一致，
匹配独立二维 oracle。65536² 程序化源以九个 tile 处理 5x7 ROI，读取 9900 字节，实际
峰值 1808 字节，保守预留 3840 字节。Focused 覆盖精确/少一字节预算、非法视图、Whole
链、fan-out、并发快照、源/sink 失败、取消和 currentness。

本地 static/shared kernel 整合及修复重跑覆盖全部 15 项注册测试，含隔离安装消费者；
daemon 两种安装形态各覆盖 15 项。C module 的专项 UBSAN no-recover 示例也通过。
受保护 Linux/macOS static/shared、ASAN/TSAN、Codex bot、合并和最终结算记录在相关
Issue/PR；本地测试不单独构成交付 gate。

Project #9 同步 CPU 叶子，daemon #15 同步 Project #15。#152 保持原生设备范围开放，
#209 机器成本标定及 #153/#154 不属于本轮 CPU 场景。S2 CPU 子集不会关闭 HEX/MED 父任务。
S3 缓存/交互与原生 GPU 继续由后续独立范围推进。

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
- [#151](https://github.com/kevin-zf1123/photospider/issues/151) 继续等待明确的 device
  storage/access、cost、liveness、transfer、residency、fallback 与真实 operation
  vertical 决策。
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
扫描和估算预算条款。Daemon 兼容维护消费安装的 0.4；新 bindings 和 bulk transport
继续由 daemon 仓库按实际需求推进。
