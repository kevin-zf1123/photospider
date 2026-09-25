---
spec_schema_version: 1
id: PTH-01
status: AcceptedDesign
implementation_status: not_implemented
registration_status: technical_freeze_gates
source_maturity: D2_draft
revised_on: 2026-09-26
---

# PTH-01：PathSet构造、分组与方向

本具体规格已按[英文权威决策D01–D12](../decisions.md)修订。数学、端口与验收目标见本文；
未实现，候选键/新schema及证明算法在[技术门禁](../freeze-gates.md)完成前不得宣称已注册。
D1/D2仅保留来源成熟度，不表示首批顺序。

## 1. 族边界

本族不解析SVG文本；消费/发布现有PathSet authority与关联字段。构造只做规定的几何转换，不吸附邻近端点。
Result outputs均以现有CompleteBundle完整封存；端口数量编译期固定，动态子路径数不是动态输出端口数。
属性默认reject_unmappable；必须明确映射才保留，不按“第几个数值”猜测属性语义。

## 2. 成员

| ID | 功能 | 对象 | 文档成熟度 |
| --- | --- | --- | --- |
| [PTH-01A](PTH-01A_make_core_path.md) | CoreVerbs路径构造 | primitive | D1_draft |
| [PTH-01B](PTH-01B_concat_paths.md) | 按端口顺序拼接路径集合 | primitive | D2_draft |
| [PTH-01C](PTH-01C_split_subpaths.md) | 按动态分组重排子路径 | primitive | D2_draft |
| [PTH-01D](PTH-01D_reverse_paths.md) | 逐子路径方向反转 | primitive | D2_draft |
| [PTH-01E](PTH-01E_make_primitive_path.md) | Primitive-authority构造 | primitive | D2_draft |

## 3. 共享约束

继承[GEN-common](GEN_common_contract.md)、适用的[随机契约](NOI_random_contract.md)及
[路径契约](PTH_geometry_contract.md)。执行按成员Regional/Halo/Whole定义，GPU按后端profile；
同版本同profile位一致，质量/数值/拓扑独立，公开Result需关联及生命周期审查。

## 4. 验收与技术门禁

成员的公式、参数、边界样例及独立数学验证必须经公开运行路径确认。
来源成熟度不代表实现；不沿用附件oracle通过记录。
具体schema、随机packing、后端、求解器及认证算法按[技术门禁](../freeze-gates.md)冻结。
