---
spec_schema_version: 1
id: NOI-04
status: AcceptedDesign
implementation_status: not_implemented
registration_status: technical_freeze_gates
source_maturity: D2_draft
revised_on: 2026-09-26
---

# NOI-04：固定Perlin与seeded二维梯度噪声

本具体规格已按[英文权威决策D01–D12](../decisions.md)修订。数学、端口与验收目标见本文；
未实现，候选键/新schema及证明算法在[技术门禁](../freeze-gates.md)完成前不得宣称已注册。
D1/D2仅保留来源成熟度，不表示首批顺序。

## 1. 族边界

A保留作者2002的数学排列/梯度；B是新的Philox格点族，不能把seed拼进A仍宣称同一序列。
二者的“strict”都是完整多项式/权重数学式正确舍入，不承诺Java逐操作double位一致。
坐标Value是采样域单位，不自动把px或normalized混入频率；映射由GEN-02/NUM显式组成。

## 2. 成员

| ID | 功能 | 对象 | 文档成熟度 |
| --- | --- | --- | --- |
| [NOI-04A](NOI-04A_perlin2002_3d.md) | Improved Perlin 2002数学版 | primitive | D1_draft |
| [NOI-04B](NOI-04B_gradient2d_philox.md) | Philox哈希二维梯度场 | primitive | D1_draft |

## 3. 共享约束

继承[GEN-common](GEN_common_contract.md)、适用的[随机契约](NOI_random_contract.md)及
[路径契约](PTH_geometry_contract.md)。执行按成员Regional/Halo/Whole定义，GPU按后端profile；
同版本同profile位一致，质量/数值/拓扑独立，公开Result需关联及生命周期审查。

## 4. 验收与技术门禁

成员的公式、参数、边界样例及独立数学验证必须经公开运行路径确认。
来源成熟度不代表实现；不沿用附件oracle通过记录。
具体schema、随机packing、后端、求解器及认证算法按[技术门禁](../freeze-gates.md)冻结。
