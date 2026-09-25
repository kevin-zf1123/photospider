---
spec_schema_version: 1
id: PTH-03
status: AcceptedDesign
implementation_status: not_implemented
registration_status: technical_freeze_gates
source_maturity: D2_draft
revised_on: 2026-09-26
---

# PTH-03：弧长与可证表

本具体规格已按[英文权威决策D01–D12](../decisions.md)修订。数学、端口与验收目标见本文；
未实现，候选键/新schema及证明算法在[技术门禁](../freeze-gates.md)完成前不得宣称已注册。
D1/D2仅保留来源成熟度，不表示首批顺序。

## 1. 族边界

length输出严格RN64真实弧长；epsilon只控制建表的几何长度包围宽度，不能降低NUM严格数值要求。
积分估计、相邻精度结果一致、control polygon近似，三者均不自动是正确舍入证书。
length与table独立输出；表布局不能随请求length与否或线程调度变化。

## 2. 成员

| ID | 功能 | 对象 | 文档成熟度 |
| --- | --- | --- | --- |
| [PTH-03A](PTH-03A_bezier_arc_length.md) | Core/Bezier长度与单调表 | primitive | D2_draft |
| [PTH-03B](PTH-03B_primitive_arc_length.md) | arc/Hermite/spline长度 | primitive | D2_draft |

## 3. 共享约束

继承[GEN-common](GEN_common_contract.md)、适用的[随机契约](NOI_random_contract.md)及
[路径契约](PTH_geometry_contract.md)。执行按成员Regional/Halo/Whole定义，GPU按后端profile；
同版本同profile位一致，质量/数值/拓扑独立，公开Result需关联及生命周期审查。

## 4. 验收与技术门禁

成员的公式、参数、边界样例及独立数学验证必须经公开运行路径确认。
来源成熟度不代表实现；不沿用附件oracle通过记录。
具体schema、随机packing、后端、求解器及认证算法按[技术门禁](../freeze-gates.md)冻结。
