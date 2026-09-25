---
spec_schema_version: 1
id: GEN-02
status: AcceptedDesign
implementation_status: not_implemented
registration_status: technical_freeze_gates
source_maturity: D2_draft
revised_on: 2026-09-26
---

# GEN-02：像素与边缘归一化坐标

本具体规格已按[英文权威决策D01–D12](../decisions.md)修订。数学、端口与验收目标见本文；
未实现，候选键/新schema及证明算法在[技术门禁](../freeze-gates.md)完成前不得宣称已注册。
D1/D2仅保留来源成熟度，不表示首批顺序。

## 1. 族边界

坐标源不依赖目标图像样本，显式canvas控制输出。
XY同时输出和单轴输出分开，旧field.coordinate测试实现及注册已退休，不提供兼容入口。
identity source map 必须使用pixel中心坐标；normalized_edge不是采样器的其他归一约定。

## 2. 成员

| ID | 功能 | 对象 | 文档成熟度 |
| --- | --- | --- | --- |
| [GEN-02A](GEN-02A_coordinates_xy.md) | 二维坐标网格 | primitive | D1_draft |
| [GEN-02B](GEN-02B_coordinate_axis.md) | 单轴坐标源 | primitive | D1_draft |

## 3. 共享约束

继承[GEN-common](GEN_common_contract.md)、适用的[随机契约](NOI_random_contract.md)及
[路径契约](PTH_geometry_contract.md)。执行按成员Regional/Halo/Whole定义，GPU按后端profile；
同版本同profile位一致，质量/数值/拓扑独立，公开Result需关联及生命周期审查。

## 4. 验收与技术门禁

成员的公式、参数、边界样例及独立数学验证必须经公开运行路径确认。
来源成熟度不代表实现；不沿用附件oracle通过记录。
具体schema、随机packing、后端、求解器及认证算法按[技术门禁](../freeze-gates.md)冻结。
