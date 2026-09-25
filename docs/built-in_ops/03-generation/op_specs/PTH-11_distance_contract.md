---
spec_schema_version: 1
id: PTH-11
status: AcceptedDesign
implementation_status: not_implemented
registration_status: technical_freeze_gates
source_maturity: D2_draft
revised_on: 2026-09-26
---

# PTH-11：中心线距离与区域SDF

本具体规格已按[英文权威决策D01–D12](../decisions.md)修订。数学、端口与验收目标见本文；
未实现，候选键/新schema及证明算法在[技术门禁](../freeze-gates.md)完成前不得宣称已注册。
D1/D2仅保留来源成熟度，不表示首批顺序。

## 1. 族边界

unsigned centerline与fill/stroke SDF不是一回事：前者到曲线所有点，后者到所定义区域的真正边界。
closest tie按(subpath,segment,t)字典序，命中边界SDF=+0；内部负，外部正，单位px。
内部重叠边不属于区域边界；min多个signed distance通常不是union的精确内部距离。

## 2. 成员

| ID | 功能 | 对象 | 文档成熟度 |
| --- | --- | --- | --- |
| [PTH-11A](PTH-11A_centerline_distance.md) | Polyline或Bezier中心线无符号距离 | primitive | D2_draft |
| [PTH-11B](PTH-11B_fill_region_sdf.md) | Polyline填充区域SDF | primitive | D2_draft |
| [PTH-11C](PTH-11C_stroke_region_sdf.md) | 精确stroke区域SDF | primitive | D2_draft |

## 3. 共享约束

继承[GEN-common](GEN_common_contract.md)、适用的[随机契约](NOI_random_contract.md)及
[路径契约](PTH_geometry_contract.md)。执行按成员Regional/Halo/Whole定义，GPU按后端profile；
同版本同profile位一致，质量/数值/拓扑独立，公开Result需关联及生命周期审查。

## 4. 验收与技术门禁

成员的公式、参数、边界样例及独立数学验证必须经公开运行路径确认。
来源成熟度不代表实现；不沿用附件oracle通过记录。
具体schema、随机packing、后端、求解器及认证算法按[技术门禁](../freeze-gates.md)冻结。
