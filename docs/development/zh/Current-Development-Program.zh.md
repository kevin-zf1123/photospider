# 当前开发计划

- 快照日期：2026-09-12
- 交付基线：ops@ffc5d0e297b9d0ea136975413d3443e23e6fa458，包 0.8.0 / ABI 8
- 当前里程碑：[#302 独立结果与 Atomic 联合执行](https://github.com/kevin-zf1123/photospider/issues/302)
- 开发分支：codex/multi-output-semantics，仅合并到 ops

GitHub Issue 为实时交付状态权威。ADR 0021 记录已接受目标，接受不表示实现完成。
基线已经包含算子基础与 G4 执行能力。历史 main@fba06270 和 ops-foundations 快照只说明
历史里程碑，本快照不审核或改变 main。ADR 0015 保持产品边界权威。

## 执行顺序

- M0 / #303：研究与契约
- M1 / #304：输出 traits 与 ABI 9
- M2 / #305：多输出编译与规划
- M3 / #306：输出独立执行、依赖和缓存
- M4 / #307：逐观察 C/C++ 结果
- M5 / #308：Atomic 联合调度
- M6 / #309：平面语义与 420
- M7 / #310：横向裁剪输出
- M8 / #311：独立通道卷积
- M9 / #312：非整数半径高斯图像与核
- M10 / #313：安装 workflow 与组合验收

各项依赖前项，分别验证并提交。全部叶项后进行新的独立全面审查、必要修复、PR 到 ops、
最终 HEAD 的现有六项 CI 与 Codex bot review、merge commit、显式 Issue 结算、本地 ops
同步及本轮分支清理。仅内核仓库在范围内。动态输出、RequestRecord 联合执行、新 Metal
算法、daemon 迁移和 #206 通道裁剪另行处理。

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
