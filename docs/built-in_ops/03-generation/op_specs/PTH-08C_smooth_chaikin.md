---
spec_schema_version: 1
id: PTH-08C
status: AcceptedDesign
implementation_status: not_implemented
registration_status: technical_freeze_gates
source_maturity: D1_draft
revised_on: 2026-09-26
---

# PTH-08C：固定迭代Chaikin平滑

本具体规格已按[英文权威决策D01–D12](../decisions.md)修订。数学、端口与验收目标见本文；
未实现，候选键/新schema及证明算法在[技术门禁](../freeze-gates.md)完成前不得宣称已注册。
D1/D2仅保留来源成熟度，不表示首批顺序。

## 1. 范围与身份

所属族：[PTH-08 简化、拟合与平滑分开](PTH-08_editing_contract.md)。本成员与同族其他成员不能隐式互换。
继承[GEN-common](GEN_common_contract.md)、[PTH-geometry](PTH_geometry_contract.md)；精度、颜色、结构表示及执行条款通过这些文件继承01/02。
来源为上传草案，经D01–D12修订；规格不代表运行时已实现。

没有商业软件黑盒bit identity或未核验GPU支持声明。

来源候选键（全部需含版本；未冻结，不是可调用API）：

- path.smooth_chaikin_v1_strict

## 3. 端口与输出推断

输入仅M/L/Z PathSet；输出Polyline PathSet。

## 4. 参数及合法域

iterations=0..12，max_vertices；open_endpoint_policy=preserve唯一v1；不支持locked内部角点（须先split）。

## 5. 数学参考与舍入

每轮每edge(P_i,P_(i+1))产生Q=(3P_i+P_(i+1))/4,R=(P_i+3P_(i+1))/4，每坐标RN64后发布为下一轮的明确阶段输入。open保留原首/尾，closed循环且不重复闭合顶点。iterations0按位恒等。该算子改变形状，不承诺指定ε或保拓扑。

## 6. 实现成本与资源

每轮点数近翻倍，完整中间owner/计费与阶段预算；O(2^k V)；RN不可融合跨轮。

## 7. Demand、发布与dirty

目标执行规则：**Whole**。完整结构/全局算法输入与关联；完成全部所选Result后封存，不发布成功前缀。
完整控制/拓扑/资源改变保守失效全部依赖区域；逐样本输入改变按实际依赖映射失效。
空Q仅执行静态preflight，不读payload、不推进随机算法。global Region、origin、owner、layout
与produced coverage真实返回。多输出仅计算所请求数学，必要共享验证不省略。
具体callback及错误影响范围须按[GEN-common](GEN_common_contract.md)验证。

## 8. 数值、错误与生命周期

继承[GEN-common](GEN_common_contract.md)：strict完整式正确舍入，accelerated最终预算不按
tap/octave累计；同版本同profile位一致。图像平面存储、合法stride/offset、预算、取消、
关联及context后保留owner规则均适用。参数/schema错误、无解、未收敛、质量失败和宿主资源
错误保持区分；有限结果不可表示为ArithmeticOverflow。无效请求不发布成功前缀。

## 10. 成员验收

0轮恒等；一段0→4第一轮0,1,3,4；closed四点变8点；不把“平滑”当保形或曲率有界。

还须验证全域与非零ROI同位、真实读取/计算范围、合法负/零stride、独立/joint输出、
低预算、取消、cache-off、参数和资源版本变化、关联验证及保留owner寿命。

## 11. 公开执行与证据边界

实现验收必须通过WorkflowDocument→Compiler→ExecutionContext公开入口及独立数学参考。
本文样例是预期结果，不是已运行记录；附件oracle及其旧通过报告不验证修订后行为。
随机成员使用新版Philox规范和新的独立向量；技术门禁未清除前不发布golden。

## 12. 依赖

来源：[S14](../research-sources.md#s14)、[S20](../research-sources.md#s20)。
继承本族契约、GEN-common及适用的NOI-random/PTH-geometry。正式字段和算法门禁不得由示例替代。
