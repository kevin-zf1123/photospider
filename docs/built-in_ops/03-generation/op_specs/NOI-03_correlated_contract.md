---
spec_schema_version: 1
id: NOI-03
status: AcceptedDesign
implementation_status: not_implemented
registration_status: technical_freeze_gates
source_maturity: D2_draft
revised_on: 2026-09-26
---

# NOI-03：卷积相关噪声及生成workflow

本具体规格已按[英文权威决策D01–D12](../decisions.md)修订。数学、端口与验收目标见本文；
未实现，候选键/新schema及证明算法在[技术门禁](../freeze-gates.md)完成前不得宣称已注册。
D1/D2仅保留来源成熟度，不表示首批顺序。

## 1. 族边界

相关噪声将“白源”和“相关核”解耦：A是可测试的数值相关filter，B组合NOI-02。
kernel按correlation方向定义，不默默翻转；两者都不能以逐tap四ULP耗尽最终预算。
均值与方差归一化不是同义词；边界重复样本破坏简单sum(w²)的单位方差保证。

## 2. 成员

| ID | 功能 | 对象 | 文档成熟度 |
| --- | --- | --- | --- |
| [NOI-03A](NOI-03A_correlate_kernel.md) | 有限核相关场 | primitive | D1_draft |
| [NOI-03B](NOI-03B_correlated_gaussian.md) | 高斯白源加显式相关核 | composite_workflow | D1_draft |

## 3. 共享约束

继承[GEN-common](GEN_common_contract.md)、适用的[随机契约](NOI_random_contract.md)及
[路径契约](PTH_geometry_contract.md)。执行按成员Regional/Halo/Whole定义，GPU按后端profile；
同版本同profile位一致，质量/数值/拓扑独立，公开Result需关联及生命周期审查。

## 4. 验收与技术门禁

成员的公式、参数、边界样例及独立数学验证必须经公开运行路径确认。
来源成熟度不代表实现；不沿用附件oracle通过记录。
具体schema、随机packing、后端、求解器及认证算法按[技术门禁](../freeze-gates.md)冻结。
