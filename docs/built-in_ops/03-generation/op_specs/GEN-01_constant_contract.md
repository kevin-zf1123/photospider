---
spec_schema_version: 1
id: GEN-01
status: AcceptedDesign
implementation_status: not_implemented
registration_status: technical_freeze_gates
source_maturity: D2_draft
revised_on: 2026-09-26
---

# GEN-01：常量 tensor 与完整颜色源

本具体规格已按[英文权威决策D01–D12](../decisions.md)修订。数学、端口与验收目标见本文；
未实现，候选键/新schema及证明算法在[技术门禁](../freeze-gates.md)完成前不得宣称已注册。
D1/D2仅保留来源成熟度，不表示首批顺序。

## 1. 族边界

raw constant 与 image constant 分开：前者是位/数值填充，后者建立 FMT 完整颜色解释。
旧测试field.constant实现及注册已退休；新成员采用动态多通道输入与完整明确的metadata规则。
不从 dtype 范围推断颜色的物理含义。随机性不参与这族。

## 2. 成员

| ID | 功能 | 对象 | 文档成熟度 |
| --- | --- | --- | --- |
| [GEN-01A](GEN-01A_constant_tensor.md) | 常量数值 tensor | primitive | D1_draft |
| [GEN-01B](GEN-01B_constant_image.md) | 完整 straight 颜色常量 | primitive | D1_draft |

## 3. 共享约束

继承[GEN-common](GEN_common_contract.md)、适用的[随机契约](NOI_random_contract.md)及
[路径契约](PTH_geometry_contract.md)。执行按成员Regional/Halo/Whole定义，GPU按后端profile；
同版本同profile位一致，质量/数值/拓扑独立，公开Result需关联及生命周期审查。

## 4. 验收与技术门禁

成员的公式、参数、边界样例及独立数学验证必须经公开运行路径确认。
来源成熟度不代表实现；不沿用附件oracle通过记录。
具体schema、随机packing、后端、求解器及认证算法按[技术门禁](../freeze-gates.md)冻结。
