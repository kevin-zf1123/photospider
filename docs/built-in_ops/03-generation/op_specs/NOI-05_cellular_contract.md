---
spec_schema_version: 1
id: NOI-05
status: AcceptedDesign
implementation_status: not_implemented
registration_status: technical_freeze_gates
source_maturity: D2_draft
revised_on: 2026-09-26
---

# NOI-05：无限格点cellular与有限点Voronoi

本具体规格已按[英文权威决策D01–D12](../decisions.md)修订。数学、端口与验收目标见本文；
未实现，候选键/新schema及证明算法在[技术门禁](../freeze-gates.md)完成前不得宣称已注册。
D1/D2仅保留来源成熟度，不表示首批顺序。

## 1. 族边界

F1/F2是在明确站点集合上的距离序统计，不能统一用邻近3x3后声称总是精确。
站点位置先按具名规则发布RN64，再对该站点集合定义几何；返回feature坐标/ID的离散tie精确。
Voronoi的F2-F1是特征场，不是Voronoi边界的通用Euclidean SDF。

## 2. 成员

| ID | 功能 | 对象 | 文档成熟度 |
| --- | --- | --- | --- |
| [NOI-05A](NOI-05A_cellular2d_l2.md) | 每格一站点的精确F1/F2 | primitive | D1_draft |
| [NOI-05B](NOI-05B_nearest_point_distances.md) | 有限点集Voronoi查询 | primitive | D1_draft |

## 3. 共享约束

继承[GEN-common](GEN_common_contract.md)、适用的[随机契约](NOI_random_contract.md)及
[路径契约](PTH_geometry_contract.md)。执行按成员Regional/Halo/Whole定义，GPU按后端profile；
同版本同profile位一致，质量/数值/拓扑独立，公开Result需关联及生命周期审查。

## 4. 验收与技术门禁

成员的公式、参数、边界样例及独立数学验证必须经公开运行路径确认。
来源成熟度不代表实现；不沿用附件oracle通过记录。
具体schema、随机packing、后端、求解器及认证算法按[技术门禁](../freeze-gates.md)冻结。
