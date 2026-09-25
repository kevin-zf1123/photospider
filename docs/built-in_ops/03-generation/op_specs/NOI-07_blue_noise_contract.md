---
spec_schema_version: 1
id: NOI-07
status: AcceptedDesign
implementation_status: not_implemented
registration_status: technical_freeze_gates
source_maturity: D2_draft
revised_on: 2026-09-26
---

# NOI-07：资源化rank场与阈值点集

本具体规格已按[英文权威决策D01–D12](../decisions.md)修订。数学、端口与验收目标见本文；
未实现，候选键/新schema及证明算法在[技术门禁](../freeze-gates.md)完成前不得宣称已注册。
D1/D2仅保留来源成熟度，不表示首批顺序。

## 1. 族边界

“蓝噪声”不能由白噪声hash或随机置换之名获得；首版先冻结资源消费接口，不伪造生产资源。
资源是canonical little-endian Int64 rank矩阵 + UTF-8 manifest（shape/版本/内容身份；具名blue/STBN另外要求生成法/许可/质量报告），
manifest的hash覆盖原始rank字节，完整校验必须先于任何输出发布。
附件4x4测试fixture不具有蓝噪声质量资格；本次不将其导入为生产资源。

## 2. 成员

| ID | 功能 | 对象 | 文档成熟度 |
| --- | --- | --- | --- |
| [NOI-07A](NOI-07A_blue_rank_tile.md) | 周期rank-tile消费 | primitive | D2_draft |
| [NOI-07B](NOI-07B_blue_threshold_points.md) | rank阈值格点分布 | primitive | D2_draft |

## 3. 共享约束

继承[GEN-common](GEN_common_contract.md)、适用的[随机契约](NOI_random_contract.md)及
[路径契约](PTH_geometry_contract.md)。执行按成员Regional/Halo/Whole定义，GPU按后端profile；
同版本同profile位一致，质量/数值/拓扑独立，公开Result需关联及生命周期审查。

## 4. 验收与技术门禁

成员的公式、参数、边界样例及独立数学验证必须经公开运行路径确认。
来源成熟度不代表实现；不沿用附件oracle通过记录。
具体schema、随机packing、后端、求解器及认证算法按[技术门禁](../freeze-gates.md)冻结。
