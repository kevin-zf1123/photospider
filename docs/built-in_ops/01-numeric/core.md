# 数值与数组基础

本页是 NUM-01～15 的入口目录。端口、参数、公式与数值类别以链接的英文族契约和成员规格为准；曲线、采样与 LUT 见[曲线、采样与 LUT](curves.md)。

## 基本术语

| 术语 | 含义 |
| --- | --- |
| 单 tensor Result | 恰含一个 Tensor 成员的 Result；成员 key 与 schema id 可任意，形状取完整 `sample_shape()`（含 batch 前缀）。 |
| `values` | 通用数值输出，schema 为 `photospider.tensor` v1、成员 `samples`，全部为普通轴，不传播 batch 拓扑。 |
| `axis` | 采样类输出附带的原子 Float64 `[3]`，内容为 `[start,end,step]`；单样本时为 `[start,start,0]`。 |
| strict | 规定的数学表达式按成员给出的舍入阶段正确舍入，结果逐位确定。 |
| accelerated | Apple Silicon CPU 与 x86-64 CPU 各自命名的 profile，满足 [accelerated 契约](op_specs/NUM_accelerated_contract.md)给出的误差界；成员可规定与 strict 逐位一致。 |
| profile key | 同一成员按 dtype、精度类别或平台区分的注册名，例如 `numeric.abs_strict`。 |

## 基础数值规定

所有 NUM 成员继承 [NUM 共同契约](op_specs/NUM_common_contract.md)和 [NUM accelerated 契约](op_specs/NUM_accelerated_contract.md)。数值域、有限性、索引合法性与输出可表示性是 unchecked 调用方前提，静态 schema、dtype、rank、参数类型与语法在准备阶段检查。要求精确和的归约与 scan 使用精确状态加最终一次舍入。允许 NaN/Inf 的成员按各自规格给出 IEEE 风格特殊值。Broadcast 与 cast 均为显式算子，二元及多输入成员要求完整 sample shape 与 dtype 匹配。

## 族目录

| 族 | 功能 | 输入 → 输出 | 规格 |
| --- | --- | --- | --- |
| NUM-01 | 表达式采样 | Float32/64 `[1]` start/end 与具名系数 → `values[N]`、`axis` | [NUM-01](op_specs/NUM-01_sample_expression.md) |
| NUM-02 | 等距序列 | 标量 Result 输入 → `values`、`axis` | [linspace](op_specs/NUM-02A_linspace.md)、[arange](op_specs/NUM-02B_arange.md) |
| NUM-03 | 常量与显式广播 | 标量或数组 + shape → `values` | [constant](op_specs/NUM-03A_constant.md)、[broadcast](op_specs/NUM-03B_broadcast.md) |
| NUM-04 | 一元函数 | 数组（rational 版本为 numerator/denominator）→ 同形 `values` | [一元契约](op_specs/NUM-04_unary_contract.md)、[三角契约](op_specs/NUM-04_trigonometric_contract.md)、[有理 π 契约](op_specs/NUM-04_rational_pi_contract.md) |
| NUM-05 | 二元算术 | 两个同 shape、同 dtype 数组 → `values` | [二元契约](op_specs/NUM-05_binary_contract.md) |
| NUM-06 | clamp / remap_range | 数据与边界数组 → `values` | [clamp](op_specs/NUM-06A_clamp.md)、[remap_range](op_specs/NUM-06B_remap_range.md) |
| NUM-07 | 比较与选择 | 数组 → UInt8 结果；`select` 按 UInt8 条件选择同 dtype 分支 | [比较契约](op_specs/NUM-07_comparison_contract.md) |
| NUM-08 | mix / smoothstep | 三个同 shape、同 dtype 浮点数组 → `values` | [mix](op_specs/NUM-08A_mix.md)、[smoothstep](op_specs/NUM-08B_smoothstep.md) |
| NUM-09 | reshape / transpose / slice | 数组（slice 另有 Int64 starts/steps）→ 保留原始位的 `values` | [reshape](op_specs/NUM-09A_reshape.md)、[transpose](op_specs/NUM-09B_transpose.md)、[slice](op_specs/NUM-09C_slice.md) |
| NUM-10 | concatenate / gather / scatter | 数组与索引 → `values` | [concatenate](op_specs/NUM-10A_concatenate.md)、[gather](op_specs/NUM-10B_gather.md)、[scatter 契约](op_specs/NUM-10_scatter_contract.md) |
| NUM-11 | 归约 | 数组 → 归约轴长度为 1 的 `values` | [归约契约](op_specs/NUM-11_reduction_contract.md) |
| NUM-12 | sort / quantile | 数组 → `values`/`indices`；数组与 `q` → quantile `values` | [sort](op_specs/NUM-12A_sort.md)、[quantile](op_specs/NUM-12B_quantile.md) |
| NUM-13 | prefix_sum / integral_image | 数组 → 单轴扫描长度为 N+1，双轴各加 1 的 `values` | [prefix_sum](op_specs/NUM-13A_prefix_sum.md)、[integral_image](op_specs/NUM-13B_integral_image.md) |
| NUM-14 | 矩阵变换 | vectors `[...,Cin]`、matrix `[Cout,Cin]`、bias `[Cout]` → `[...,Cout]` | [NUM-14](op_specs/NUM-14_matrix_transform.md) |
| NUM-15 | 一维微分与积分 | `[N]` 样本与 step/initial → `values` | [derivative_1d](op_specs/NUM-15A_derivative_1d.md)、[integrate_1d](op_specs/NUM-15B_integrate_1d.md) |

## 逐成员入口

| ID / spec | 类别 | 注册 key | 实现 |
| --- | --- | --- | --- |
| [NUM-01 — sample_expression](op_specs/NUM-01_sample_expression.md) | primitive / D1_draft | `numeric.sample_expression_strict`<br>`numeric.sample_expression_accelerated_apple_silicon`<br>`numeric.sample_expression_accelerated_x86_64` | 已实现 |
| [NUM-02A — linspace](op_specs/NUM-02A_linspace.md) | primitive / D1_draft | `numeric.linspace_strict`<br>`numeric.linspace_accelerated_apple_silicon`<br>`numeric.linspace_accelerated_x86_64` | 已实现 |
| [NUM-02B — arange](op_specs/NUM-02B_arange.md) | primitive / D1_draft | `numeric.arange_strict`<br>`numeric.arange_accelerated_apple_silicon`<br>`numeric.arange_accelerated_x86_64` | 已实现 |
| [NUM-03A — constant](op_specs/NUM-03A_constant.md) | primitive / D1_draft | `numeric.constant_strict`<br>`numeric.constant_accelerated_apple_silicon`<br>`numeric.constant_accelerated_x86_64` | 已实现 |
| [NUM-03B — broadcast](op_specs/NUM-03B_broadcast.md) | primitive / D1_draft | `numeric.broadcast_strict`<br>`numeric.broadcast_accelerated_apple_silicon`<br>`numeric.broadcast_accelerated_x86_64` | 已实现 |
| [NUM-04A — abs](op_specs/NUM-04A_abs.md) | primitive / D1_draft | `numeric.abs_strict`<br>`numeric.abs_accelerated_apple_silicon`<br>`numeric.abs_accelerated_x86_64` | 已实现 |
| [NUM-04B — neg](op_specs/NUM-04B_neg.md) | primitive / D1_draft | `numeric.neg_strict`<br>`numeric.neg_accelerated_apple_silicon`<br>`numeric.neg_accelerated_x86_64` | 已实现 |
| [NUM-04C — sqrt](op_specs/NUM-04C_sqrt.md) | primitive / D1_draft | `numeric.sqrt_strict`<br>`numeric.sqrt_accelerated_apple_silicon`<br>`numeric.sqrt_accelerated_x86_64` | 已实现 |
| [NUM-04D — exp](op_specs/NUM-04D_exp.md) | primitive / D1_draft | `numeric.exp_strict`<br>`numeric.exp_accelerated_apple_silicon`<br>`numeric.exp_accelerated_x86_64` | 已实现 |
| [NUM-04E — ln](op_specs/NUM-04E_ln.md) | primitive / D1_draft | `numeric.ln_strict`<br>`numeric.ln_accelerated_apple_silicon`<br>`numeric.ln_accelerated_x86_64` | 已实现 |
| [NUM-04F — sin](op_specs/NUM-04F_sin.md) | primitive / D1_draft | `numeric.sin_strict`<br>`numeric.sin_accelerated_apple_silicon`<br>`numeric.sin_accelerated_x86_64` | 已实现 |
| [NUM-04G — cos](op_specs/NUM-04G_cos.md) | primitive / D1_draft | `numeric.cos_strict`<br>`numeric.cos_accelerated_apple_silicon`<br>`numeric.cos_accelerated_x86_64` | 已实现 |
| [NUM-04H — tan](op_specs/NUM-04H_tan.md) | primitive / D1_draft | `numeric.tan_strict`<br>`numeric.tan_accelerated_apple_silicon`<br>`numeric.tan_accelerated_x86_64` | 已实现 |
| [NUM-04I — floor](op_specs/NUM-04I_floor.md) | primitive / D1_draft | `numeric.floor_strict`<br>`numeric.floor_accelerated_apple_silicon`<br>`numeric.floor_accelerated_x86_64` | 已实现 |
| [NUM-04J — ceil](op_specs/NUM-04J_ceil.md) | primitive / D1_draft | `numeric.ceil_strict`<br>`numeric.ceil_accelerated_apple_silicon`<br>`numeric.ceil_accelerated_x86_64` | 已实现 |
| [NUM-04K — round](op_specs/NUM-04K_round.md) | primitive / D1_draft | `numeric.round_strict`<br>`numeric.round_accelerated_apple_silicon`<br>`numeric.round_accelerated_x86_64` | 已实现 |
| [NUM-04L — sign](op_specs/NUM-04L_sign.md) | primitive / D1_draft | `numeric.sign_strict`<br>`numeric.sign_accelerated_apple_silicon`<br>`numeric.sign_accelerated_x86_64` | 已实现 |
| [NUM-04M — reciprocal](op_specs/NUM-04M_reciprocal.md) | primitive / D1_draft | `numeric.reciprocal_strict`<br>`numeric.reciprocal_accelerated_apple_silicon`<br>`numeric.reciprocal_accelerated_x86_64` | 已实现 |
| [NUM-04N — sinpi](op_specs/NUM-04N_sinpi.md) | primitive / D1_draft | `numeric.sinpi_strict`<br>`numeric.sinpi_accelerated_apple_silicon`<br>`numeric.sinpi_accelerated_x86_64` | 已实现 |
| [NUM-04O — cospi](op_specs/NUM-04O_cospi.md) | primitive / D1_draft | `numeric.cospi_strict`<br>`numeric.cospi_accelerated_apple_silicon`<br>`numeric.cospi_accelerated_x86_64` | 已实现 |
| [NUM-04P — tanpi](op_specs/NUM-04P_tanpi.md) | primitive / D1_draft | `numeric.tanpi_strict`<br>`numeric.tanpi_accelerated_apple_silicon`<br>`numeric.tanpi_accelerated_x86_64` | 已实现 |
| [NUM-04Q — sinc](op_specs/NUM-04Q_sinc.md) | primitive / D1_draft | `numeric.sinc_strict`<br>`numeric.sinc_accelerated_apple_silicon`<br>`numeric.sinc_accelerated_x86_64` | 已实现 |
| [NUM-04R — sincpi](op_specs/NUM-04R_sincpi.md) | primitive / D1_draft | `numeric.sincpi_strict`<br>`numeric.sincpi_accelerated_apple_silicon`<br>`numeric.sincpi_accelerated_x86_64` | 已实现 |
| [NUM-04S — sinpi_rational](op_specs/NUM-04S_sinpi_rational.md) | primitive / D1_draft | `numeric.sinpi_rational_strict`<br>`numeric.sinpi_rational_accelerated_apple_silicon`<br>`numeric.sinpi_rational_accelerated_x86_64` | 已实现 |
| [NUM-04T — cospi_rational](op_specs/NUM-04T_cospi_rational.md) | primitive / D1_draft | `numeric.cospi_rational_strict`<br>`numeric.cospi_rational_accelerated_apple_silicon`<br>`numeric.cospi_rational_accelerated_x86_64` | 已实现 |
| [NUM-04U — tanpi_rational](op_specs/NUM-04U_tanpi_rational.md) | primitive / D1_draft | `numeric.tanpi_rational_strict`<br>`numeric.tanpi_rational_accelerated_apple_silicon`<br>`numeric.tanpi_rational_accelerated_x86_64` | 已实现 |
| [NUM-04V — sincpi_rational](op_specs/NUM-04V_sincpi_rational.md) | primitive / D1_draft | `numeric.sincpi_rational_strict`<br>`numeric.sincpi_rational_accelerated_apple_silicon`<br>`numeric.sincpi_rational_accelerated_x86_64` | 已实现 |
| [NUM-05A — add](op_specs/NUM-05A_add.md) | primitive / D1_draft | `numeric.add_strict`<br>`numeric.add_accelerated_apple_silicon`<br>`numeric.add_accelerated_x86_64` | 已实现 |
| [NUM-05B — subtract](op_specs/NUM-05B_subtract.md) | primitive / D1_draft | `numeric.subtract_strict`<br>`numeric.subtract_accelerated_apple_silicon`<br>`numeric.subtract_accelerated_x86_64` | 已实现 |
| [NUM-05C — multiply](op_specs/NUM-05C_multiply.md) | primitive / D1_draft | `numeric.multiply_strict`<br>`numeric.multiply_accelerated_apple_silicon`<br>`numeric.multiply_accelerated_x86_64` | 已实现 |
| [NUM-05D — divide](op_specs/NUM-05D_divide.md) | primitive / D1_draft | `numeric.divide_strict`<br>`numeric.divide_accelerated_apple_silicon`<br>`numeric.divide_accelerated_x86_64` | 已实现 |
| [NUM-05E — minimum](op_specs/NUM-05E_minimum.md) | primitive / D1_draft | `numeric.minimum_strict`<br>`numeric.minimum_accelerated_apple_silicon`<br>`numeric.minimum_accelerated_x86_64` | 已实现 |
| [NUM-05F — maximum](op_specs/NUM-05F_maximum.md) | primitive / D1_draft | `numeric.maximum_strict`<br>`numeric.maximum_accelerated_apple_silicon`<br>`numeric.maximum_accelerated_x86_64` | 已实现 |
| [NUM-05G — pow](op_specs/NUM-05G_pow.md) | primitive / D1_draft | `numeric.pow_strict`<br>`numeric.pow_accelerated_apple_silicon`<br>`numeric.pow_accelerated_x86_64` | 已实现 |
| [NUM-05H — atan2](op_specs/NUM-05H_atan2.md) | primitive / D1_draft | `numeric.atan2_strict`<br>`numeric.atan2_accelerated_apple_silicon`<br>`numeric.atan2_accelerated_x86_64` | 已实现 |
| [NUM-05I — atan2pi](op_specs/NUM-05I_atan2pi.md) | primitive / D1_draft | `numeric.atan2pi_strict`<br>`numeric.atan2pi_accelerated_apple_silicon`<br>`numeric.atan2pi_accelerated_x86_64` | 已实现 |
| [NUM-06A — clamp](op_specs/NUM-06A_clamp.md) | primitive / D1_draft | `numeric.clamp_strict`<br>`numeric.clamp_accelerated_apple_silicon`<br>`numeric.clamp_accelerated_x86_64` | 已实现 |
| [NUM-06B — remap_range](op_specs/NUM-06B_remap_range.md) | primitive / D1_draft | `numeric.remap_range_strict`<br>`numeric.remap_range_accelerated_apple_silicon`<br>`numeric.remap_range_accelerated_x86_64` | 已实现 |
| [NUM-07A — equal](op_specs/NUM-07A_equal.md) | primitive / D1_draft | `numeric.equal_strict`<br>`numeric.equal_accelerated_apple_silicon`<br>`numeric.equal_accelerated_x86_64` | 已实现 |
| [NUM-07B — not_equal](op_specs/NUM-07B_not_equal.md) | primitive / D1_draft | `numeric.not_equal_strict`<br>`numeric.not_equal_accelerated_apple_silicon`<br>`numeric.not_equal_accelerated_x86_64` | 已实现 |
| [NUM-07C — less](op_specs/NUM-07C_less.md) | primitive / D1_draft | `numeric.less_strict`<br>`numeric.less_accelerated_apple_silicon`<br>`numeric.less_accelerated_x86_64` | 已实现 |
| [NUM-07D — less_equal](op_specs/NUM-07D_less_equal.md) | primitive / D1_draft | `numeric.less_equal_strict`<br>`numeric.less_equal_accelerated_apple_silicon`<br>`numeric.less_equal_accelerated_x86_64` | 已实现 |
| [NUM-07E — greater](op_specs/NUM-07E_greater.md) | primitive / D1_draft | `numeric.greater_strict`<br>`numeric.greater_accelerated_apple_silicon`<br>`numeric.greater_accelerated_x86_64` | 已实现 |
| [NUM-07F — greater_equal](op_specs/NUM-07F_greater_equal.md) | primitive / D1_draft | `numeric.greater_equal_strict`<br>`numeric.greater_equal_accelerated_apple_silicon`<br>`numeric.greater_equal_accelerated_x86_64` | 已实现 |
| [NUM-07G — is_close](op_specs/NUM-07G_is_close.md) | primitive / D1_draft | `numeric.is_close_strict`<br>`numeric.is_close_accelerated_apple_silicon`<br>`numeric.is_close_accelerated_x86_64` | 已实现 |
| [NUM-07H — select](op_specs/NUM-07H_select.md) | primitive / D1_draft | `numeric.select_strict`<br>`numeric.select_accelerated_apple_silicon`<br>`numeric.select_accelerated_x86_64` | 已实现 |
| [NUM-08A — mix](op_specs/NUM-08A_mix.md) | primitive / D1_draft | `numeric.mix_strict`<br>`numeric.mix_accelerated_apple_silicon`<br>`numeric.mix_accelerated_x86_64` | 已实现 |
| [NUM-08B — smoothstep](op_specs/NUM-08B_smoothstep.md) | primitive / D1_draft | `numeric.smoothstep_strict`<br>`numeric.smoothstep_accelerated_apple_silicon`<br>`numeric.smoothstep_accelerated_x86_64` | 已实现 |
| [NUM-09A — reshape](op_specs/NUM-09A_reshape.md) | primitive / D1_draft | `array.reshape_strict`<br>`array.reshape_accelerated_apple_silicon`<br>`array.reshape_accelerated_x86_64` | 已实现 |
| [NUM-09B — transpose](op_specs/NUM-09B_transpose.md) | primitive / D1_draft | `array.transpose_strict`<br>`array.transpose_accelerated_apple_silicon`<br>`array.transpose_accelerated_x86_64` | 已实现 |
| [NUM-09C — slice](op_specs/NUM-09C_slice.md) | primitive / D1_draft | `array.slice_strict`<br>`array.slice_accelerated_apple_silicon`<br>`array.slice_accelerated_x86_64` | 已实现 |
| [NUM-10A — concatenate](op_specs/NUM-10A_concatenate.md) | primitive / D1_draft | `array.concatenate_strict`<br>`array.concatenate_accelerated_apple_silicon`<br>`array.concatenate_accelerated_x86_64` | 已实现 |
| [NUM-10B — gather](op_specs/NUM-10B_gather.md) | primitive / D1_draft | `array.gather_strict`<br>`array.gather_accelerated_apple_silicon`<br>`array.gather_accelerated_x86_64` | 已实现 |
| [NUM-10C — scatter_replace](op_specs/NUM-10C_scatter_replace.md) | primitive / D1_draft | `array.scatter_replace_strict`<br>`array.scatter_replace_accelerated_apple_silicon`<br>`array.scatter_replace_accelerated_x86_64` | 已实现 |
| [NUM-10D — scatter_sum](op_specs/NUM-10D_scatter_sum.md) | primitive / D1_draft | `array.scatter_sum_strict`<br>`array.scatter_sum_accelerated_apple_silicon`<br>`array.scatter_sum_accelerated_x86_64` | 已实现 |
| [NUM-10E — scatter_minimum](op_specs/NUM-10E_scatter_minimum.md) | primitive / D1_draft | `array.scatter_minimum_strict`<br>`array.scatter_minimum_accelerated_apple_silicon`<br>`array.scatter_minimum_accelerated_x86_64` | 已实现 |
| [NUM-10F — scatter_maximum](op_specs/NUM-10F_scatter_maximum.md) | primitive / D1_draft | `array.scatter_maximum_strict`<br>`array.scatter_maximum_accelerated_apple_silicon`<br>`array.scatter_maximum_accelerated_x86_64` | 已实现 |
| [NUM-11A — reduce_sum](op_specs/NUM-11A_reduce_sum.md) | primitive / D1_draft | `numeric.reduce_sum_strict`<br>`numeric.reduce_sum_accelerated_apple_silicon`<br>`numeric.reduce_sum_accelerated_x86_64` | 已实现 |
| [NUM-11B — reduce_minimum](op_specs/NUM-11B_reduce_minimum.md) | primitive / D1_draft | `numeric.reduce_minimum_strict`<br>`numeric.reduce_minimum_accelerated_apple_silicon`<br>`numeric.reduce_minimum_accelerated_x86_64` | 已实现 |
| [NUM-11C — reduce_maximum](op_specs/NUM-11C_reduce_maximum.md) | primitive / D1_draft | `numeric.reduce_maximum_strict`<br>`numeric.reduce_maximum_accelerated_apple_silicon`<br>`numeric.reduce_maximum_accelerated_x86_64` | 已实现 |
| [NUM-11D — reduce_mean](op_specs/NUM-11D_reduce_mean.md) | primitive / D1_draft | `numeric.reduce_mean_strict`<br>`numeric.reduce_mean_accelerated_apple_silicon`<br>`numeric.reduce_mean_accelerated_x86_64` | 已实现 |
| [NUM-11E — reduce_count](op_specs/NUM-11E_reduce_count.md) | primitive / D1_draft | `numeric.reduce_count_strict`<br>`numeric.reduce_count_accelerated_apple_silicon`<br>`numeric.reduce_count_accelerated_x86_64` | 已实现 |
| [NUM-11F — reduce_variance](op_specs/NUM-11F_reduce_variance.md) | primitive / D1_draft | `numeric.reduce_variance_strict`<br>`numeric.reduce_variance_accelerated_apple_silicon`<br>`numeric.reduce_variance_accelerated_x86_64` | 已实现 |
| [NUM-11G — reduce_std](op_specs/NUM-11G_reduce_std.md) | primitive / D1_draft | `numeric.reduce_std_strict`<br>`numeric.reduce_std_accelerated_apple_silicon`<br>`numeric.reduce_std_accelerated_x86_64` | 已实现 |
| [NUM-12A — sort](op_specs/NUM-12A_sort.md) | primitive / D1_draft | `array.sort_strict`<br>`array.sort_accelerated_apple_silicon`<br>`array.sort_accelerated_x86_64` | 已实现 |
| [NUM-12B — quantile](op_specs/NUM-12B_quantile.md) | primitive / D1_draft | `numeric.quantile_strict`<br>`numeric.quantile_accelerated_apple_silicon`<br>`numeric.quantile_accelerated_x86_64` | 已实现 |
| [NUM-13A — prefix_sum](op_specs/NUM-13A_prefix_sum.md) | primitive / D1_draft | `numeric.prefix_sum_strict`<br>`numeric.prefix_sum_accelerated_apple_silicon`<br>`numeric.prefix_sum_accelerated_x86_64` | 已实现 |
| [NUM-13B — integral_image](op_specs/NUM-13B_integral_image.md) | primitive / D1_draft | `numeric.integral_image_strict`<br>`numeric.integral_image_accelerated_apple_silicon`<br>`numeric.integral_image_accelerated_x86_64` | 已实现 |
| [NUM-14 — matrix_transform](op_specs/NUM-14_matrix_transform.md) | primitive / D1_draft | `numeric.matrix_transform_strict`<br>`numeric.matrix_transform_accelerated_apple_silicon`<br>`numeric.matrix_transform_accelerated_x86_64` | 已实现 |
| [NUM-15A — derivative_1d](op_specs/NUM-15A_derivative_1d.md) | primitive / D1_draft | `numeric.derivative_1d_strict`<br>`numeric.derivative_1d_accelerated_apple_silicon`<br>`numeric.derivative_1d_accelerated_x86_64` | 已实现 |
| [NUM-15B — integrate_1d](op_specs/NUM-15B_integrate_1d.md) | primitive / D1_draft | `numeric.integrate_1d_strict`<br>`numeric.integrate_1d_accelerated_apple_silicon`<br>`numeric.integrate_1d_accelerated_x86_64` | 已实现 |

## 相关入口

- [曲线、采样与 LUT](curves.md)
- [公共数据约定](../00-foundation/contracts.md)
- [数学 oracle](../../../oracle/ops/numeric/README.md)
