---
spec_schema_version: 1
id: GEN-05
status: AcceptedDesign
implementation_status: not_implemented
registration_status: technical_freeze_gates
source_maturity: D2_draft
revised_on: 2026-09-26
---

# GEN-05：渐变坐标：几何与颜色分离

本具体规格已按[英文权威决策D01–D12](../decisions.md)修订。数学、端口与验收目标见本文；
未实现，候选键/新schema及证明算法在[技术门禁](../freeze-gates.md)完成前不得宣称已注册。
D1/D2仅保留来源成熟度，不表示首批顺序。

## 1. 族边界

输出t是无量纲scalar field，不含颜色、spread、clipping或alpha。
point p使用GEN-common的全局像素中心；数学式的 geometry均有限。
linear两端重合和非正radius拒绝，不继承SVG的退化为末色规则。
A–E是D1解析几何；F依赖path距离；G明确两个圆的根选择与无解掩码。

## 2. 成员

| ID | 功能 | 对象 | 文档成熟度 |
| --- | --- | --- | --- |
| [GEN-05A](GEN-05A_linear_gradient_coordinate.md) | 线性渐变坐标 | primitive | D1_draft |
| [GEN-05B](GEN-05B_radial_gradient_coordinate.md) | 圆与轴对齐椭圆径向坐标 | primitive | D1_draft |
| [GEN-05C](GEN-05C_angular_gradient_coordinate.md) | 顺时针角向坐标 | primitive | D1_draft |
| [GEN-05D](GEN-05D_diamond_gradient_coordinate.md) | L1菱形坐标 | primitive | D1_draft |
| [GEN-05E](GEN-05E_box_gradient_coordinate.md) | L∞方框坐标 | primitive | D1_draft |
| [GEN-05F](GEN-05F_path_distance_coordinate.md) | 路径距离渐变坐标 | composite_workflow | D2_draft |
| [GEN-05G](GEN-05G_two_circle_radial_coordinate.md) | 双圆径向根选择 | primitive | D2_draft |

## 3. 共享约束

继承[GEN-common](GEN_common_contract.md)、适用的[随机契约](NOI_random_contract.md)及
[路径契约](PTH_geometry_contract.md)。执行按成员Regional/Halo/Whole定义，GPU按后端profile；
同版本同profile位一致，质量/数值/拓扑独立，公开Result需关联及生命周期审查。

## 4. 验收与技术门禁

成员的公式、参数、边界样例及独立数学验证必须经公开运行路径确认。
来源成熟度不代表实现；不沿用附件oracle通过记录。
具体schema、随机packing、后端、求解器及认证算法按[技术门禁](../freeze-gates.md)冻结。
