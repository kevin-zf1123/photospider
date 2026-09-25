---
spec_schema_version: 1
id: GEN-04
status: AcceptedDesign
implementation_status: not_implemented
registration_status: technical_freeze_gates
source_maturity: D2_draft
revised_on: 2026-09-26
---

# GEN-04：可解析测试图案

本具体规格已按[英文权威决策D01–D12](../decisions.md)修订。数学、端口与验收目标见本文；
未实现，候选键/新schema及证明算法在[技术门禁](../freeze-gates.md)完成前不得宣称已注册。
D1/D2仅保留来源成熟度，不表示首批顺序。

## 1. 族边界

全部是点采样诊断源，不默认带AA；测试重采样时必须知道输入自身的频谱/采样方式。
color bars是明确的8条full-range线性RGB诊断条，不冒称SMPTE/EBU编码测试信号。
不同pattern具名，避免一个不透明String mode隐含参数解释。

## 2. 成员

| ID | 功能 | 对象 | 文档成熟度 |
| --- | --- | --- | --- |
| [GEN-04A](GEN-04A_checker_pattern.md) | 棋盘格 | primitive | D1_draft |
| [GEN-04B](GEN-04B_grid_pattern.md) | 规则网格线 | primitive | D1_draft |
| [GEN-04C](GEN-04C_ramp_pattern.md) | 端点已知的离散测试斜坡 | primitive | D1_draft |
| [GEN-04D](GEN-04D_impulse_pattern.md) | 像素索引脉冲 | primitive | D1_draft |
| [GEN-04E](GEN-04E_zone_plate.md) | 径向扫频 zone plate | primitive | D1_draft |
| [GEN-04F](GEN-04F_siemens_star.md) | Siemens星形角向余弦图案 | primitive | D1_draft |
| [GEN-04G](GEN-04G_linear_rgb_bars.md) | 显式线性RGB八色条 | primitive | D1_draft |

## 3. 共享约束

继承[GEN-common](GEN_common_contract.md)、适用的[随机契约](NOI_random_contract.md)及
[路径契约](PTH_geometry_contract.md)。执行按成员Regional/Halo/Whole定义，GPU按后端profile；
同版本同profile位一致，质量/数值/拓扑独立，公开Result需关联及生命周期审查。

## 4. 验收与技术门禁

成员的公式、参数、边界样例及独立数学验证必须经公开运行路径确认。
来源成熟度不代表实现；不沿用附件oracle通过记录。
具体schema、随机packing、后端、求解器及认证算法按[技术门禁](../freeze-gates.md)冻结。
