---
spec_schema_version: 1
id: PTH-08B
status: AcceptedDesign
implementation_status: not_implemented
registration_status: technical_freeze_gates
source_maturity: D2_draft
revised_on: 2026-09-26
---

# PTH-08B：有连续几何验收的三次拟合

本具体规格已按[英文权威决策D01–D12](../decisions.md)修订。数学、端口与验收目标见本文；
未实现，候选键/新schema及证明算法在[技术门禁](../freeze-gates.md)完成前不得宣称已注册。
D1/D2仅保留来源成熟度，不表示首批顺序。

## 1. 范围与身份

所属族：[PTH-08 简化、拟合与平滑分开](PTH-08_editing_contract.md)。本成员与同族其他成员不能隐式互换。
继承[GEN-common](GEN_common_contract.md)、[PTH-geometry](PTH_geometry_contract.md)；精度、颜色、结构表示及执行条款通过这些文件继承01/02。
来源为上传草案，经D01–D12修订；规格不代表运行时已实现。

输出不唯一且求解器未冻结；本成员不提议可立即注册的operation key，渲染时标registration gate。

公开键按语义成员、算法版本及profile冻结；不存在未版本化兼容入口。

## 3. 端口与输出推断

输入有序PointSet/Polyline（显式subpath分组）、corner/locked字段；输出Core cubic PathSet与fit report逻辑Result。

## 4. 参数及合法域

epsilon_geom_px>0，max_segments，max_iterations，source_topology_policy=allow_change默认，可选reject_unproven；endpoint_tangent_policy=chord/explicit required。

## 5. 数学参考与舍入

每个后端求解器定义独立版本化算子，共享以下可观察验收：端点/locked点命中；拟合曲线与原折线双向Hausdorff≤ε，geometry publication error计入；corner处断开切线约束。建议chord-length参数→有约束least-squares→保护重参数→最大残差分裂，但采样残差不是连续证书。各求解器的初始化、tie、迭代、分段停止及失败行为须独立冻结；不同求解器可产生不同合格输出，同算子/版本/profile必须位一致。不得以mode或运行时回退静默更换求解器。

## 6. 实现成本与资源

候选O(iterations*V)每段，连续曲线-折线距离证书需interval subdivision；失败可增加段直到预算，不可悄悄改ε或返回最佳guess。

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

直线可精确cubic退化；corner保留；离散点全命中但中间overshoot必须失败；自交/小环认证；相同输入决定性需算法冻结后验收。

还须验证全域与非零ROI同位、真实读取/计算范围、合法负/零stride、独立/joint输出、
低预算、取消、cache-off、参数和资源版本变化、关联验证及保留owner寿命。

## 11. 公开执行与证据边界

实现验收必须通过WorkflowDocument→Compiler→ExecutionContext公开入口及独立数学参考。
本文样例是预期结果，不是已运行记录；附件oracle及其旧通过报告不验证修订后行为。
随机成员使用新版Philox规范和新的独立向量；技术门禁未清除前不发布golden。

## 12. 依赖

来源：[S14](../research-sources.md#s14)、[S20](../research-sources.md#s20)。
继承本族契约、GEN-common及适用的NOI-random/PTH-geometry。正式字段和算法门禁不得由示例替代。
