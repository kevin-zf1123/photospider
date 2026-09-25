---
spec_schema_version: 1
id: GEN-08
status: AcceptedDesign
implementation_status: not_implemented
registration_status: technical_freeze_gates
source_maturity: D2_draft
revised_on: 2026-09-26
---

# GEN-08：规则、扰动与最小距离点集

本具体规格已按[英文权威决策D01–D12](../decisions.md)修订。数学、端口与验收目标见本文；
未实现，候选键/新schema及证明算法在[技术门禁](../freeze-gates.md)完成前不得宣称已注册。
D1/D2仅保留来源成熟度，不表示首批顺序。

## 1. 族边界

输出统一使用现有 DynamicPoints/PointSetSpec，dimensions=2、Ordinal IDs、attribute_width=1，
属性唯一分量固定1（未定义额外物理含义），id_to_row完整双射；位置Float64，空count合法。
minimum distance是在实际发布Float64点上用精确平方距离检验，不能只验证舍入前候选。
A/B按nx*ny生成完整网格，使用独立硬容量限制；C/D的max_count为正常停止目标，达到即成功，算法先耗尽则返回较少点及停止原因。硬资源预算耗尽仍失败。
C是有限候选hard-core rejection，D是Bridson具名版本；都不承诺最大/最密点集。

## 2. 成员

| ID | 功能 | 对象 | 文档成熟度 |
| --- | --- | --- | --- |
| [GEN-08A](GEN-08A_grid_points.md) | 规则中心点分布 | primitive | D1_draft |
| [GEN-08B](GEN-08B_jittered_grid_points.md) | 每格独立均匀扰动 | primitive | D1_draft |
| [GEN-08C](GEN-08C_poisson_rejection_points.md) | 有限候选最小距离分布 | primitive | D1_draft |
| [GEN-08D](GEN-08D_bridson_points.md) | Bridson-annulus具名分布 | primitive | D2_draft |

## 3. 共享约束

继承[GEN-common](GEN_common_contract.md)、适用的[随机契约](NOI_random_contract.md)及
[路径契约](PTH_geometry_contract.md)。执行按成员Regional/Halo/Whole定义，GPU按后端profile；
同版本同profile位一致，质量/数值/拓扑独立，公开Result需关联及生命周期审查。

## 4. 验收与技术门禁

成员的公式、参数、边界样例及独立数学验证必须经公开运行路径确认。
来源成熟度不代表实现；不沿用附件oracle通过记录。
具体schema、随机packing、后端、求解器及认证算法按[技术门禁](../freeze-gates.md)冻结。
