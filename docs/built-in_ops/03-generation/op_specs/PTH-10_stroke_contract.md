---
spec_schema_version: 1
id: PTH-10
status: AcceptedDesign
implementation_status: not_implemented
registration_status: technical_freeze_gates
source_maturity: D2_draft
revised_on: 2026-09-26
---

# PTH-10：恒宽、变宽与轮廓输出

本具体规格已按[英文权威决策D01–D12](../decisions.md)修订。数学、端口与验收目标见本文；
未实现，候选键/新schema及证明算法在[技术门禁](../freeze-gates.md)完成前不得宣称已注册。
D1/D2仅保留来源成熟度，不表示首批顺序。

## 1. 族边界

A/B直接定义精确stroke区域的coverage；C是单独具名的polygonized outline输出，不假装与A/B同一几何。
width为直径，不把0宽做设备hairline；重叠先求区域union再coverage，不能重复source-over。
v1 centerline为已发布Polyline；曲线源必须前置具名flatten，误差要保留。

## 2. 成员

| ID | 功能 | 对象 | 文档成熟度 |
| --- | --- | --- | --- |
| [PTH-10A](PTH-10A_stroke_constant_area.md) | 恒宽Polyline描边面积 | primitive | D2_draft |
| [PTH-10B](PTH-10B_stroke_variable_round_area.md) | 线性变宽round-sweep面积 | primitive | D2_draft |
| [PTH-10C](PTH-10C_stroke_outline_flatten.md) | 有界polygonized描边轮廓 | primitive | D2_draft |

## 3. 共享约束

继承[GEN-common](GEN_common_contract.md)、适用的[随机契约](NOI_random_contract.md)及
[路径契约](PTH_geometry_contract.md)。执行按成员Regional/Halo/Whole定义，GPU按后端profile；
同版本同profile位一致，质量/数值/拓扑独立，公开Result需关联及生命周期审查。

## 4. 验收与技术门禁

成员的公式、参数、边界样例及独立数学验证必须经公开运行路径确认。
来源成熟度不代表实现；不沿用附件oracle通过记录。
具体schema、随机packing、后端、求解器及认证算法按[技术门禁](../freeze-gates.md)冻结。
