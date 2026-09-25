---
spec_schema_version: 1
id: PTH-02
status: AcceptedDesign
implementation_status: not_implemented
registration_status: technical_freeze_gates
source_maturity: D2_draft
revised_on: 2026-09-26
---

# PTH-02：位置、导数和单位切线

本具体规格已按[英文权威决策D01–D12](../decisions.md)修订。数学、端口与验收目标见本文；
未实现，候选键/新schema及证明算法在[技术门禁](../freeze-gates.md)完成前不得宣称已注册。
D1/D2仅保留来源成熟度，不表示首批顺序。

## 1. 族边界

所有输出分别命名position[N,2]、derivative[N,2]、tangent[N,2]和tangent_valid[N] UInt8。
导数相对归一局部t，单位为px/t；单位切线是d/sqrt(d·d)，d=0时(+0,+0),valid=0，否则valid=1。
position-only无需算切线平方根，但完整输入/全部query预校验不省略；选择的segment/t离散一致。

## 2. 成员

| ID | 功能 | 对象 | 文档成熟度 |
| --- | --- | --- | --- |
| [PTH-02A](PTH-02A_evaluate_bezier_path.md) | 绝对控制Bézier求值 | primitive | D1_draft |
| [PTH-02B](PTH-02B_evaluate_elliptic_arc.md) | 参数椭圆弧求值 | primitive | D2_draft |
| [PTH-02C](PTH-02C_evaluate_hermite_path.md) | 三次Hermite求值 | primitive | D1_draft |
| [PTH-02D](PTH-02D_evaluate_bspline_path.md) | 非周期多项式/有理B-spline | primitive | D2_draft |
| [PTH-02E](PTH-02E_evaluate_pathset.md) | PathSet分派workflow | composite_workflow | D2_draft |

## 3. 共享约束

继承[GEN-common](GEN_common_contract.md)、适用的[随机契约](NOI_random_contract.md)及
[路径契约](PTH_geometry_contract.md)。执行按成员Regional/Halo/Whole定义，GPU按后端profile；
同版本同profile位一致，质量/数值/拓扑独立，公开Result需关联及生命周期审查。

## 4. 验收与技术门禁

成员的公式、参数、边界样例及独立数学验证必须经公开运行路径确认。
来源成熟度不代表实现；不沿用附件oracle通过记录。
具体schema、随机packing、后端、求解器及认证算法按[技术门禁](../freeze-gates.md)冻结。
