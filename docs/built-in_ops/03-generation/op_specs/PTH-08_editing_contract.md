---
spec_schema_version: 1
id: PTH-08
status: AcceptedDesign
implementation_status: not_implemented
registration_status: technical_freeze_gates
source_maturity: D2_draft
revised_on: 2026-09-26
---

# PTH-08：简化、拟合与平滑分开

本具体规格已按[英文权威决策D01–D12](../decisions.md)修订。数学、端口与验收目标见本文；
未实现，候选键/新schema及证明算法在[技术门禁](../freeze-gates.md)完成前不得宣称已注册。
D1/D2仅保留来源成熟度，不表示首批顺序。

## 1. 族边界

“减少点数”“拟合曲线”“改变形状”三类目标不能共用一个error_tol模糊处理。
所有误差默认Euclidean px；保拓扑必须独立证明。按本草稿未提供证明的算法不得自动标preserve_topology=true。
首版输出CoreVerbs；attrs无法一一映射则reject/drop，corner/locked点需要显式输入标签。

## 2. 成员

| ID | 功能 | 对象 | 文档成熟度 |
| --- | --- | --- | --- |
| [PTH-08A](PTH-08A_simplify_polyline.md) | 参数对应有界的折线简化 | primitive | D2_draft |
| [PTH-08B](PTH-08B_fit_cubic_segments.md) | 有连续几何验收的三次拟合 | primitive | D2_draft |
| [PTH-08C](PTH-08C_smooth_chaikin.md) | 固定迭代Chaikin平滑 | primitive | D1_draft |

## 3. 共享约束

继承[GEN-common](GEN_common_contract.md)、适用的[随机契约](NOI_random_contract.md)及
[路径契约](PTH_geometry_contract.md)。执行按成员Regional/Halo/Whole定义，GPU按后端profile；
同版本同profile位一致，质量/数值/拓扑独立，公开Result需关联及生命周期审查。

## 4. 验收与技术门禁

成员的公式、参数、边界样例及独立数学验证必须经公开运行路径确认。
来源成熟度不代表实现；不沿用附件oracle通过记录。
具体schema、随机packing、后端、求解器及认证算法按[技术门禁](../freeze-gates.md)冻结。
