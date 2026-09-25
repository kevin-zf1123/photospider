---
spec_schema_version: 1
id: GEN-03
status: AcceptedDesign
implementation_status: not_implemented
registration_status: technical_freeze_gates
source_maturity: D2_draft
revised_on: 2026-09-26
---

# GEN-03：基础图形：coverage、路径与SDF

本具体规格已按[英文权威决策D01–D12](../decisions.md)修订。数学、端口与验收目标见本文；
未实现，候选键/新schema及证明算法在[技术门禁](../freeze-gates.md)完成前不得宣称已注册。
D1/D2仅保留来源成熟度，不表示首批顺序。

## 1. 族边界

先生成coverage/几何，再显式着色；不混入fill color、source-over或图层系统。
A/B是面积coverage，不使用中心距离smoothstep。C复用PTH-09A；D生成星形PathSet，再选择fill/distance。
E/F是实际欧氏距离，不把归一ellipse半径减1冒充px SDF。

## 2. 成员

| ID | 功能 | 对象 | 文档成熟度 |
| --- | --- | --- | --- |
| [GEN-03A](GEN-03A_rectangle_coverage.md) | 轴对齐矩形面积覆盖 | primitive | D1_draft |
| [GEN-03B](GEN-03B_ellipse_coverage.md) | 椭圆面积覆盖 | primitive | D2_draft |
| [GEN-03C](GEN-03C_polygon_coverage.md) | 多轮廓多边形填充 | composite_workflow | D1_draft |
| [GEN-03D](GEN-03D_star_path.md) | 规则交替半径星形路径 | primitive | D2_draft |
| [GEN-03E](GEN-03E_rectangle_sdf.md) | 矩形欧氏有符号距离 | primitive | D1_draft |
| [GEN-03F](GEN-03F_ellipse_sdf.md) | 椭圆欧氏有符号距离 | primitive | D2_draft |

## 3. 共享约束

继承[GEN-common](GEN_common_contract.md)、适用的[随机契约](NOI_random_contract.md)及
[路径契约](PTH_geometry_contract.md)。执行按成员Regional/Halo/Whole定义，GPU按后端profile；
同版本同profile位一致，质量/数值/拓扑独立，公开Result需关联及生命周期审查。

## 4. 验收与技术门禁

成员的公式、参数、边界样例及独立数学验证必须经公开运行路径确认。
来源成熟度不代表实现；不沿用附件oracle通过记录。
具体schema、随机packing、后端、求解器及认证算法按[技术门禁](../freeze-gates.md)冻结。
