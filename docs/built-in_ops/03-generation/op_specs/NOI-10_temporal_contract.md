---
spec_schema_version: 1
id: NOI-10
status: AcceptedDesign
implementation_status: not_implemented
registration_status: technical_freeze_gates
source_maturity: D2_draft
revised_on: 2026-09-26
---

# NOI-10：连续时间场、平流与STBN

本具体规格已按[英文权威决策D01–D12](../decisions.md)修订。数学、端口与验收目标见本文；
未实现，候选键/新schema及证明算法在[技术门禁](../freeze-gates.md)完成前不得宣称已注册。
D1/D2仅保留来源成熟度，不表示首批顺序。

## 1. 族边界

随机frame变化、沿空间移动和连续时间坐标是三种不同语义。A/B固定底场seed，time作为数学坐标；
不得每帧更换seed制造不连续跳变。C使用单独时空资源，不把二维蓝噪声逐帧平移叫作STBN。
时间是有限Float64秒，速度明确为坐标单位/秒；缓存包含time原始位与资源身份。

## 2. 成员

| ID | 功能 | 对象 | 文档成熟度 |
| --- | --- | --- | --- |
| [NOI-10A](NOI-10A_advected_gradient2d.md) | 二维固定场平流 | primitive | D1_draft |
| [NOI-10B](NOI-10B_temporal_perlin3d.md) | 时间作为第三维的连续场 | primitive | D1_draft |
| [NOI-10C](NOI-10C_stbn_rank_volume.md) | 时空rank资源消费 | primitive | D2_draft |

## 3. 共享约束

继承[GEN-common](GEN_common_contract.md)、适用的[随机契约](NOI_random_contract.md)及
[路径契约](PTH_geometry_contract.md)。执行按成员Regional/Halo/Whole定义，GPU按后端profile；
同版本同profile位一致，质量/数值/拓扑独立，公开Result需关联及生命周期审查。

## 4. 验收与技术门禁

成员的公式、参数、边界样例及独立数学验证必须经公开运行路径确认。
来源成熟度不代表实现；不沿用附件oracle通过记录。
具体schema、随机packing、后端、求解器及认证算法按[技术门禁](../freeze-gates.md)冻结。
