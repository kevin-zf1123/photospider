---
spec_schema_version: 1
id: NOI-08
status: AcceptedDesign
implementation_status: not_implemented
registration_status: technical_freeze_gates
source_maturity: D2_draft
revised_on: 2026-09-26
---

# NOI-08：Poisson计数与电子shot模型

本具体规格已按[英文权威决策D01–D12](../decisions.md)修订。数学、端口与验收目标见本文；
未实现，候选键/新schema及证明算法在[技术门禁](../freeze-gates.md)完成前不得宣称已注册。
D1/D2仅保留来源成熟度，不表示首批顺序。

## 1. 族边界

A定义稳定的Poisson inverse-CDF，而不复用库随版本改变的抽样算法。
B把均值单位限定为电子，艺术grain、读取噪声、增益、量化与裁剪均留在显式后继节点。
小λ下大量0是合法输出，不是随机失败；返回计数的离散分支没有accelerated误差容忍。

## 2. 成员

| ID | 功能 | 对象 | 文档成熟度 |
| --- | --- | --- | --- |
| [NOI-08A](NOI-08A_poisson_icdf.md) | 有限open52驱动的Poisson量化 | primitive | D2_draft |
| [NOI-08B](NOI-08B_shot_electrons.md) | 电子计数shot workflow | composite_workflow | D2_draft |

## 3. 共享约束

继承[GEN-common](GEN_common_contract.md)、适用的[随机契约](NOI_random_contract.md)及
[路径契约](PTH_geometry_contract.md)。执行按成员Regional/Halo/Whole定义，GPU按后端profile；
同版本同profile位一致，质量/数值/拓扑独立，公开Result需关联及生命周期审查。

## 4. 验收与技术门禁

成员的公式、参数、边界样例及独立数学验证必须经公开运行路径确认。
来源成熟度不代表实现；不沿用附件oracle通过记录。
具体schema、随机packing、后端、求解器及认证算法按[技术门禁](../freeze-gates.md)冻结。
