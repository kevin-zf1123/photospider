---
spec_schema_version: 1
id: PTH-05
status: AcceptedDesign
implementation_status: not_implemented
registration_status: technical_freeze_gates
source_maturity: D2_draft
revised_on: 2026-09-26
---

# PTH-05：整路径宽度曲线与现有属性

本具体规格已按[英文权威决策D01–D12](../decisions.md)修订。数学、端口与验收目标见本文；
未实现，候选键/新schema及证明算法在[技术门禁](../freeze-gates.md)完成前不得宣称已注册。
D1/D2仅保留来源成熟度，不表示首批顺序。

## 1. 族边界

width固定为直径px，非负。默认整PathSet实际s/L，一个profile跨subpaths按长度串接且不计跳跃。
PCHIP仅是查询计算；PathAttributeInterpolation v1只有Constant/Linear，不能新增整数3冒充已支持PCHIP。
复制/附着width必须使用具名属性描述，不能把任意ArcLength attr识别为width。

## 2. 成员

| ID | 功能 | 对象 | 文档成熟度 |
| --- | --- | --- | --- |
| [PTH-05A](PTH-05A_width_profile_linear.md) | 分段线性宽度查询 | primitive | D1_draft |
| [PTH-05B](PTH-05B_width_profile_pchip.md) | PCHIP宽度查询workflow | composite_workflow | D2_draft |
| [PTH-05C](PTH-05C_attach_linear_width.md) | 附着现有线性ArcLength宽度属性 | primitive | D2_draft |

## 3. 共享约束

继承[GEN-common](GEN_common_contract.md)、适用的[随机契约](NOI_random_contract.md)及
[路径契约](PTH_geometry_contract.md)。执行按成员Regional/Halo/Whole定义，GPU按后端profile；
同版本同profile位一致，质量/数值/拓扑独立，公开Result需关联及生命周期审查。

## 4. 验收与技术门禁

成员的公式、参数、边界样例及独立数学验证必须经公开运行路径确认。
来源成熟度不代表实现；不沿用附件oracle通过记录。
具体schema、随机packing、后端、求解器及认证算法按[技术门禁](../freeze-gates.md)冻结。
