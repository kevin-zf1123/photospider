---
spec_schema_version: 1
id: PTH-06
status: AcceptedDesign
implementation_status: not_implemented
registration_status: technical_freeze_gates
source_maturity: D2_draft
revised_on: 2026-09-26
---

# PTH-06：中心线和已描边轮廓的不同变换

本具体规格已按[英文权威决策D01–D12](../decisions.md)修订。数学、端口与验收目标见本文；
未实现，候选键/新schema及证明算法在[技术门禁](../freeze-gates.md)完成前不得宣称已注册。
D1/D2仅保留来源成熟度，不表示首批顺序。

## 1. 族边界

相同仿射矩阵作用于不同几何对象不是同一个视觉结果。中心线先变换再固定px描边，width不跟随矩阵；
轮廓先构造再变换会把圆cap变成椭圆，非均匀缩放时两者必须区别。
首版仅仿射2x3，不偷接perspective；投影变换须另具名处理无穷远、符号与切割。

## 2. 成员

| ID | 功能 | 对象 | 文档成熟度 |
| --- | --- | --- | --- |
| [PTH-06A](PTH-06A_transform_centerline.md) | 路径中心线仿射变换 | primitive | D2_draft |
| [PTH-06B](PTH-06B_transform_stroke_outline.md) | 已构造描边轮廓变换workflow | composite_workflow | D2_draft |

## 3. 共享约束

继承[GEN-common](GEN_common_contract.md)、适用的[随机契约](NOI_random_contract.md)及
[路径契约](PTH_geometry_contract.md)。执行按成员Regional/Halo/Whole定义，GPU按后端profile；
同版本同profile位一致，质量/数值/拓扑独立，公开Result需关联及生命周期审查。

## 4. 验收与技术门禁

成员的公式、参数、边界样例及独立数学验证必须经公开运行路径确认。
来源成熟度不代表实现；不沿用附件oracle通过记录。
具体schema、随机packing、后端、求解器及认证算法按[技术门禁](../freeze-gates.md)冻结。
