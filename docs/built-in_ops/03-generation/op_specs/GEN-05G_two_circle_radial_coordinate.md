---
spec_schema_version: 1
id: GEN-05G
status: AcceptedDesign
implementation_status: not_implemented
registration_status: technical_freeze_gates
source_maturity: D2_draft
revised_on: 2026-09-26
---

# GEN-05G：双圆径向根选择

本具体规格已按[英文权威决策D01–D12](../decisions.md)修订。数学、端口与验收目标见本文；
未实现，候选键/新schema及证明算法在[技术门禁](../freeze-gates.md)完成前不得宣称已注册。
D1/D2仅保留来源成熟度，不表示首批顺序。

## 1. 范围与身份

所属族：[GEN-05 渐变坐标：几何与颜色分离](GEN-05_gradient_coordinates_contract.md)。本成员与同族其他成员不能隐式互换。
继承[GEN-common](GEN_common_contract.md)、[PTH-geometry](PTH_geometry_contract.md)；精度、颜色、结构表示及执行条款通过这些文件继承01/02。
来源为上传草案，经D01–D12修订；规格不代表运行时已实现。

没有商业软件黑盒bit identity或未核验GPU支持声明。

来源候选键（全部需含版本；未冻结，不是可调用API）：

- generation.two_circle_radial_coordinate_v1_strict
  - generation.two_circle_radial_coordinate_v1_accelerated_apple_silicon
  - generation.two_circle_radial_coordinate_v1_accelerated_x86_64

## 3. 端口与输出推断

输入c0[2],r0[1],c1[2],r1[1]；输出t[H,W]浮点、valid[H,W] UInt8。

## 4. 参数及合法域

canvas,dtype=float64；r0,r1≥0且不是相同圆；no_solution=reject/valid_zero，默认valid_zero。

## 5. 数学参考与舍入

设q=p-c0,d=c1-c0,s=r1-r0，解A t²+B t+C=0，A=d·d-s²,B=-2(q·d+r0*s),C=q·q-r0²。筛r0+t*s≥0的实根，取最大t。A=0按线性；A=B=C=0时检查r0+t*s≥0：s<0选有限最大t=-r0/s；s≥0无有限最大坐标，沿no_solution策略处理。无合格根或无有限最大坐标时valid_zero时t=+0,valid=0，否则失败；成功valid=1。

## 6. 实现成本与资源

O(P)，判别式/根筛选exact，稳定quadratic仅是候选；根正确舍入需interval。t不限[0,1]。

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

同心r0=0,r1=2，距中心1得到t=.5；无实根、切重根、A=0；valid单独请求一致。

还须验证全域与非零ROI同位、真实读取/计算范围、合法负/零stride、独立/joint输出、
低预算、取消、cache-off、参数和资源版本变化、关联验证及保留owner寿命。

## 11. 公开执行与证据边界

实现验收必须通过WorkflowDocument→Compiler→ExecutionContext公开入口及独立数学参考。
本文样例是预期结果，不是已运行记录；附件oracle及其旧通过报告不验证修订后行为。
随机成员使用新版Philox规范和新的独立向量；技术门禁未清除前不发布golden。

## 12. 依赖

来源：[S01](../research-sources.md#s01)。
继承本族契约、GEN-common及适用的NOI-random/PTH-geometry。正式字段和算法门禁不得由示例替代。
