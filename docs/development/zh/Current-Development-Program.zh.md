# 当前开发计划

- 快照日期：2026-09-25。
- 已核验远端基线：`ops@61517151c0d0d2a2c0a70f52965350130eeda3a0`，包0.24.0。
  本快照不审核或改变 `main`。
- 本地已审核实现：`99cc11126759a59deeb91f55c56c3443ce1bd28a`，核验时未推送。
  下述CTest维护是额外的本地工作树修改，不表示已经远端交付。
- GitHub Issue拥有交付状态，Project反映剩余范围。授权边界见
  [任务协作](Task-Collaboration.zh.md)，产品权威仍是ADR0015；接受规格不构成实现证据。

## 已实现基线与本地增量

远端基线包含公开typed compile/plan/execute流水线、独立输出与Atomic联合执行、
CPU区域及分阶段执行、本地派生缓存、原生Metal存储/dispatch、planar图像存储。
维护中的格式能力包含通道提取/组装、通道编辑组合、严格数值转换及元数据赋值。

[#302独立结果里程碑](https://github.com/kevin-zf1123/photospider/issues/302)
及#303–#313均已关闭。[PR #314](https://github.com/kevin-zf1123/photospider/pull/314)
在`575a424d`合入ops。其审核/CI/合并流程是历史完成证据，不是每个新任务的强制流程。

本地`99cc1112`增加sealed planar preparation复用、显式BitwiseMapped值关系、
公开run/copy helper及使用现有SIMD的generic转换。版本轴保持独立：traits18、
semantic/physical v16、C operation ABI9、provider ABI1、WorkflowDocument3、TDM4。
包仍为0.24.0，C++消费者必须整体重编译。优化器仍是`optimizer-v5-canonical-noop`；
这些运行时优化没有实现跨节点融合或增量编译，参见英文Compiler Version Contract。

## 正确性测试清单审查

已按行为审查原79个默认CTest及条件SME测试，默认清单调整为78项：

- 退休历史13个已删除格式key的名单测试，将虚构未知算子的查询/调用/编译拒绝保留在
  `test_compiler`。
- 五个G4及三个foundations注册项按实际依赖、GPU、数值、表达式和滤波行为改名。
- 退休不合法generic-image STMap正例和计时入口，保留其余稀疏/依赖/缓存workflow。
  Planar STMap仍未实现。
- GPU fixture分别验证普通取消与先发生的Protocol错误，两者均检查零发布。
- 已注册可执行文件随默认构建生成；SME不可用记为skip。安装运行覆盖仅包含嵌套
  run target的实际命令，不能将所有可选consumer都计为已执行。

本地修改后的完整static CTest：macOS为77通过/1失败；FreeBSD为70通过/1失败/
7个Metal跳过，各78项。条件SME测试在本机Apple M5实际执行通过。以上是本地结果，
不是远端CI状态。剩余`test_execution`暴露动态opaque facet传播缺陷：缓存契约允许
通用Drop及其PreserveInput链，但调用验证将运行facet与静态元数据比较。该测试继续
注册，未删除或放宽断言。当前入口见[测试与验证](Testing-and-Validation.zh.md)。

此前`99cc1112`的原生static/shared及sanitizer审查同时记录了基线失败，以及不链接
Photospider也可复现的FreeBSD `__thr_calloc` TSan报告。精确suppression不等于
无过滤TSan通过；LeakSanitizer不受支持。本轮清单维护不改写这些限制，也不声称
重新完成sanitizer矩阵。

## 剩余内核计划

- Foundations [#139](https://github.com/kevin-zf1123/photospider/issues/139)：
  #143及#246/#247/#248的operation/provider starter和可选manifest仍未完成；
  当前正确性失败仍需修复。
- Compiler [#145](https://github.com/kevin-zf1123/photospider/issues/145)、
  [#147](https://github.com/kevin-zf1123/photospider/issues/147)：#148 structured
  explain/validator增量，随后#149保守pass，再到#203可丢弃增量重编译。
  S1输入契约已接受并完成；#148需要剩余实现范围triage，不需要再次等待S1批准。
  #150仍为后续明确限定的变换计划。
- Heterogeneous [#151](https://github.com/kevin-zf1123/photospider/issues/151)、
  [#152](https://github.com/kevin-zf1123/photospider/issues/152)：CPU区域叶项与
  #153/#154/#156原生驻留、执行和诊断已交付。#209成本单位及planner消费的机器profile仍未完成。
- Calibration [#155](https://github.com/kevin-zf1123/photospider/issues/155)：
  #212/#213保留。已有原生原始测量不等于完成校准profile或自动选址，不能因generic
  SIMD吞吐结果关闭优化/校准父项。

Daemon Session/Job、IPC与WebUI交付由各自仓库负责，本次内核测试清单任务没有重新审核。
历史G/S阶段名不定义当前CTest名称，也不恢复退休图像契约。

## 更新规则

已审核基线、当前里程碑、关键路径或阻塞变化时刷新本快照，并引用实际代码、测试及
交付位置。Issue保留范围与历史，Project反映其状态。接受提案不表示实现，只有本地
工作的任务不得标为远端完成。普通实现细节保留在所属Issue和测试中。
