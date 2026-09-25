---
spec_schema_version: 1
id: PTH-09B
status: AcceptedDesign
implementation_status: not_implemented
registration_status: technical_freeze_gates
source_maturity: D2_draft
revised_on: 2026-09-26
---

# PTH-09B：明确flatten-v1的曲线fill workflow

本具体规格已按[英文权威决策D01–D12](../decisions.md)修订。数学、端口与验收目标见本文；
未实现，候选键/新schema及证明算法在[技术门禁](../freeze-gates.md)完成前不得宣称已注册。
D1/D2仅保留来源成熟度，不表示首批顺序。

## 1. 范围与身份

所属族：[PTH-09 多边形面积与具名折线化填充](PTH-09_fill_contract.md)。本成员与同族其他成员不能隐式互换。
继承[GEN-common](GEN_common_contract.md)、[PTH-geometry](PTH_geometry_contract.md)；精度、颜色、结构表示及执行条款通过这些文件继承01/02。
来源为上传草案，经D01–D12修订；规格不代表运行时已实现。

没有商业软件黑盒bit identity或未核验GPU支持声明。

公开键按语义成员、算法版本及profile冻结；不存在未版本化兼容入口。

## 3. 端口与输出推断

输入Core/Bezier PathSet；输出coverage[H,W] Float32/64；可选geometry_report独立逻辑Result。

## 4. 参数及合法域

canvas,dtype,fill_rule；epsilon_geom_px默认.05及PTH-04D预算；source_topology_policy=allow_change/reject_unproven，默认allow_change。

## 5. 数学参考与舍入

严格阶段PTH-04D发布Polyline后，执行A对该Polyline面积正确舍入。source_topology_policy=allow_change允许细分近似改变微小孔洞/相切结构；未验证时报告未验证，仅证实变化才报告变化；reject_unproven必须另证拓扑，否则InvalidQuality。不能写成原Bezier面积误差≤.05。

## 6. 实现成本与资源

完整polyline中间backing及source→polyline证书计费；宽ε不能当提升数值4ULP预算的理由。

## 7. Demand、发布与dirty

目标执行规则：**Regional**。按Q计算输出；坐标/查询Value按所需样本读取。小型控制、stop/table、路径/mesh拓扑和资源描述完整验证；依赖全路径时可以完整读取路径，但不因此计算全画布。
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

直线退化与A完全相同；曲线epsilon不同可改几何；自交靠近切点的拓扑gate；全域与精确ROI结果同位，并验证ROI实际计算与内存范围。

还须验证全域与非零ROI同位、真实读取/计算范围、合法负/零stride、独立/joint输出、
低预算、取消、cache-off、参数和资源版本变化、关联验证及保留owner寿命。

## 11. 公开执行与证据边界

实现验收必须通过WorkflowDocument→Compiler→ExecutionContext公开入口及独立数学参考。
本文样例是预期结果，不是已运行记录；附件oracle及其旧通过报告不验证修订后行为。
随机成员使用新版Philox规范和新的独立向量；技术门禁未清除前不发布golden。

## 12. 依赖

来源：[S03](../research-sources.md#s03)、[S14](../research-sources.md#s14)、[S15](../research-sources.md#s15)。
继承本族契约、GEN-common及适用的NOI-random/PTH-geometry。正式字段和算法门禁不得由示例替代。
