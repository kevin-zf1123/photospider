# 曲线、采样与 LUT

本页是 CRV-01～11 的入口目录。端口、参数、公式与数值类别以链接的英文族契约和成员规格为准；数组与算术基础见[数值与数组基础](core.md)。

## 基本术语

| 术语 | 含义 |
| --- | --- |
| 独立 1D 函数 | `[N,C]` 表中每一列是一条以同一自变量索引的函数，例如 RGB 三条独立曲线。 |
| color ramp | 标量 t 到颜色的映射；输出 `sample_shape(input)+[C]`，颜色组由显式 TensorDescription 解释。 |
| 三维 LUT | `[Nr,Ng,Nb,3]` 表，三个颜色分量共同索引；与 `[N,3]` 的独立 1D 函数或 color ramp 语义不同。[^clf] |
| `start,end,count` 采样 | count≥2 时包含两端并支持递减区间；count=1 只在 start 求值，不读取 end；返回的 `axis` 为 `[start,end,step]`。 |
| 控制点 | anchors、relative handles、x/y 节点与 LUT 表都是显式数据输入，不从文件隐式读取。 |
| Lab/LCh 亮度 | 颜色描述中的 CIELAB/CIELCh 亮度统一使用 `l=L*/100`；LCh/HSL 的 hue 保留原始值与圈数，直接插值，不归一化。 |

## 基础数值规定

CRV 继承 [NUM 共同契约](op_specs/NUM_common_contract.md)和 [NUM accelerated 契约](op_specs/NUM_accelerated_contract.md)；strict 与 Apple Silicon CPU、x86-64 CPU 的 accelerated profile 分别命名。曲线输出默认不 clip 到 `[0,1]`，需要截断时显式连接 clamp。控制点有限性、x 递增、单调拓扑、段索引合法性、LUT 轴有效性与输出可表示性为 unchecked 调用方前提。一般反函数由 CRV-10 求解原数学曲线，不能通过倒置数组次序得到。

CRV-01、CRV-02、CRV-04、CRV-05 另有显式 `fast` profile，遵循各族的原生数值规则；其误差与舍入承诺见对应英文规格。[CRV-05C](op_specs/CRV-05C_apply_lut1d_vector.md)的单 query 多列 LUT fast 入口为待实现规格，供 [GRD-06C](../07-grade/op_specs/GRD-06C_gradient_map_channels.md)复用。

## 族目录

| 族 | 功能 | 输入 → 输出 | 规格 |
| --- | --- | --- | --- |
| CRV-01 | 插值 | x`[K]`、y`[K]` 或 `[K,C]`、query`[N]` → `values` `[N]` 或 `[N,C]`；linear 与 PCHIP，单函数与多函数各为独立算子 | [CRV-01](op_specs/CRV-01_interpolate.md) |
| CRV-02 | Bézier 函数采样 | anchors`[K,2]`、relative handles、start/end → `values[count]`、`axis`<br>`curve.sample_bezier_function_fast` | [CRV-02](op_specs/CRV-02_sample_bezier_function.md) |
| CRV-03 | 参数曲线求值 | anchors`[K,D]`、relative handles、Int64 segment_indices`[N]`、t`[N]` → `values[N,D]` | [CRV-03](op_specs/CRV-03_evaluate_bezier.md) |
| CRV-04 | LUT1D 烘焙模板 | 六种函数来源 + start/end/count → `values`、`axis` | [CRV-04](op_specs/CRV-04_bake_lut1d.md) |
| CRV-05 | LUT1D 应用 | input、table `[L]` 或 `[L,C]`、axis → 同 shape，或标量 query 扩展为向量输出 | [CRV-05](op_specs/CRV-05_apply_lut1d.md) |
| CRV-06 | color ramp | input、stops、colors（RationalPi 另有 Int64 分子/分母）→ `sample_shape(input)+[C]` | [CRV-06](op_specs/CRV-06_color_ramp.md) |
| CRV-07 | 三轴 LUT3D 应用 | input`[...,3]`、table`[N0,N1,N2,3]`、axis`[3,3]` → 保持 input shape；trilinear 与 tetrahedral 独立 | [CRV-07](op_specs/CRV-07_apply_lut3d.md) |
| CRV-08 | 坐标 shaper | input 与同 dtype lower/upper → 同 shape `values`；线性与 log2 正反变换 | [CRV-08](op_specs/CRV-08_shaper.md) |
| CRV-09 | LUT3D 烘焙模板 | 点式同模型颜色变换 + axis + 可选 validation_points → table 与 Measured 报告，带显式质量门 | [CRV-09](op_specs/CRV-09_bake_lut3d.md) |
| CRV-10 | 一维反函数 | x`[K]`、y`[K]`、query`[N]` → `values[N]` | [CRV-10](op_specs/CRV-10_invert.md) |
| CRV-11 | 信号重采样与抗混叠低通 | uniform `input` 或 nonuniform `positions`/`values` → 重采样结果 | [重采样](op_specs/CRV-11_resample_signal.md)、[uniform 低通](op_specs/CRV-11_uniform_lowpass_contract.md)、[nonuniform 低通](op_specs/CRV-11_nonuniform_lowpass_contract.md) |

`curve.sample_linear` 与 `curve.sample_monotone` 接收 `[K,2]` 控制点，与 CRV-02 的 Bézier 函数采样属于不同算子，接口见 [Basic Operations](../../kernel-architecture/Basic-Operations.md)。

## 插值选择

| 方法 | 连续性和控制 | 适用 / 取舍 |
| --- | --- | --- |
| linear | C0，局部、无区间过冲 | 首版最容易预测；宽度与折线调色 |
| PCHIP | C1、保持单调形状，二阶导数可跳 | 单调tone、非负width；不替代几何B-spline |
| cubic spline | 常见C2；边界natural/clamped/not-a-knot/periodic显式 | 更平滑，可能overshoot |
| cubic Hermite | 端点值+切线 | 艺术控制、动画缓动；切线可导致越界 |
| B-spline | degree、knots决定局部支撑与连续性 | 几何和拟合；控制点通常不在曲线上 |
| Fourier | 全局周期基、均匀采样假设 | 明确周期的轮廓/宽度调制；非周期端点可振铃 |

PCHIP 与 B-spline 的性质可对照 SciPy 官方定义；Fourier resample 假设信号周期。[^pchip][^bspline][^resample] 普通 tone 或 width 曲线不宜默认使用全局高阶多项式拟合，局部编辑会影响全域并可能产生振荡。

验收示例：x² 在 0、0.5、1 取 0、0.25、1；烘焙这三个点后线性查表 0.25 得 0.125，体现离散表近似。

## 逐成员入口

| ID / spec | 类别 | 注册 key | 实现 |
| --- | --- | --- | --- |
| [CRV-01A — interpolate_linear](op_specs/CRV-01A_interpolate_linear.md) | primitive / D1_draft | `curve.interpolate_linear_strict`<br>`curve.interpolate_linear_accelerated_apple_silicon`<br>`curve.interpolate_linear_accelerated_x86_64`<br>`curve.interpolate_linear_fast` | 已实现 |
| [CRV-01B — interpolate_pchip](op_specs/CRV-01B_interpolate_pchip.md) | primitive / D1_draft | `curve.interpolate_pchip_strict`<br>`curve.interpolate_pchip_accelerated_apple_silicon`<br>`curve.interpolate_pchip_accelerated_x86_64`<br>`curve.interpolate_pchip_fast` | 已实现 |
| [CRV-01C — interpolate_linear_multi](op_specs/CRV-01C_interpolate_linear_multi.md) | primitive / D1_draft | `curve.interpolate_linear_multi_strict`<br>`curve.interpolate_linear_multi_accelerated_apple_silicon`<br>`curve.interpolate_linear_multi_accelerated_x86_64`<br>`curve.interpolate_linear_multi_fast` | 已实现 |
| [CRV-01D — interpolate_pchip_multi](op_specs/CRV-01D_interpolate_pchip_multi.md) | primitive / D1_draft | `curve.interpolate_pchip_multi_strict`<br>`curve.interpolate_pchip_multi_accelerated_apple_silicon`<br>`curve.interpolate_pchip_multi_accelerated_x86_64`<br>`curve.interpolate_pchip_multi_fast` | 已实现 |
| [CRV-02 — sample_bezier_function](op_specs/CRV-02_sample_bezier_function.md) | primitive / D1_draft | `curve.sample_bezier_function_strict`<br>`curve.sample_bezier_function_accelerated_apple_silicon`<br>`curve.sample_bezier_function_accelerated_x86_64`<br>`curve.sample_bezier_function_fast` | 已实现 |
| [CRV-03 — evaluate_bezier](op_specs/CRV-03_evaluate_bezier.md) | primitive / D1_draft | `curve.evaluate_bezier_strict`<br>`curve.evaluate_bezier_accelerated_apple_silicon`<br>`curve.evaluate_bezier_accelerated_x86_64` | 已实现 |
| [CRV-04A — bake_lut1d_expression](op_specs/CRV-04A_bake_lut1d_expression.md) | composite_workflow / D1_draft | — | 已实现 |
| [CRV-04B — bake_lut1d_bezier](op_specs/CRV-04B_bake_lut1d_bezier.md) | composite_workflow / D1_draft | — | 已实现 |
| [CRV-04C — bake_lut1d_linear](op_specs/CRV-04C_bake_lut1d_linear.md) | composite_workflow / D1_draft | — | 已实现 |
| [CRV-04D — bake_lut1d_pchip](op_specs/CRV-04D_bake_lut1d_pchip.md) | composite_workflow / D1_draft | — | 已实现 |
| [CRV-04E — bake_lut1d_linear_multi](op_specs/CRV-04E_bake_lut1d_linear_multi.md) | composite_workflow / D1_draft | — | 已实现 |
| [CRV-04F — bake_lut1d_pchip_multi](op_specs/CRV-04F_bake_lut1d_pchip_multi.md) | composite_workflow / D1_draft | — | 已实现 |
| [CRV-05A — apply_lut1d](op_specs/CRV-05A_apply_lut1d.md) | primitive / D1_draft | `curve.apply_lut1d_strict`<br>`curve.apply_lut1d_accelerated_apple_silicon`<br>`curve.apply_lut1d_accelerated_x86_64`<br>`curve.apply_lut1d_fast` | 已实现 |
| [CRV-05B — apply_lut1d_channels](op_specs/CRV-05B_apply_lut1d_channels.md) | primitive / D1_draft | `curve.apply_lut1d_channels_strict`<br>`curve.apply_lut1d_channels_accelerated_apple_silicon`<br>`curve.apply_lut1d_channels_accelerated_x86_64`<br>`curve.apply_lut1d_channels_fast` | 已实现 |
| [CRV-05C — apply_lut1d_vector](op_specs/CRV-05C_apply_lut1d_vector.md) | primitive / D1_draft | `curve.apply_lut1d_vector_fast`（提议） | 待实现 |
| [CRV-06A — color_ramp_rgb](op_specs/CRV-06A_color_ramp_rgb.md) | primitive / D1_draft | `curve.color_ramp_rgb_strict`<br>`curve.color_ramp_rgb_accelerated_apple_silicon`<br>`curve.color_ramp_rgb_accelerated_x86_64` | 已实现 |
| [CRV-06B — color_ramp_cmyk](op_specs/CRV-06B_color_ramp_cmyk.md) | primitive / D1_draft | `curve.color_ramp_cmyk_strict`<br>`curve.color_ramp_cmyk_accelerated_apple_silicon`<br>`curve.color_ramp_cmyk_accelerated_x86_64` | 已实现 |
| [CRV-06C — color_ramp_cielab](op_specs/CRV-06C_color_ramp_cielab.md) | primitive / D1_draft | `curve.color_ramp_cielab_strict`<br>`curve.color_ramp_cielab_accelerated_apple_silicon`<br>`curve.color_ramp_cielab_accelerated_x86_64` | 已实现 |
| [CRV-06D1 — color_ramp_cielch](op_specs/CRV-06D1_color_ramp_cielch.md) | primitive / D1_draft | `curve.color_ramp_cielch_strict`<br>`curve.color_ramp_cielch_accelerated_apple_silicon`<br>`curve.color_ramp_cielch_accelerated_x86_64` | 已实现 |
| [CRV-06D2 — color_ramp_cielch_pi](op_specs/CRV-06D2_color_ramp_cielch_pi.md) | primitive / D1_draft | `curve.color_ramp_cielch_pi_strict`<br>`curve.color_ramp_cielch_pi_accelerated_apple_silicon`<br>`curve.color_ramp_cielch_pi_accelerated_x86_64` | 已实现 |
| [CRV-06D3 — color_ramp_cielch_rational_pi](op_specs/CRV-06D3_color_ramp_cielch_rational_pi.md) | primitive / D1_draft | `curve.color_ramp_cielch_rational_pi_strict`<br>`curve.color_ramp_cielch_rational_pi_accelerated_apple_silicon`<br>`curve.color_ramp_cielch_rational_pi_accelerated_x86_64` | 已实现 |
| [CRV-06E — color_ramp_oklab](op_specs/CRV-06E_color_ramp_oklab.md) | primitive / D1_draft | `curve.color_ramp_oklab_strict`<br>`curve.color_ramp_oklab_accelerated_apple_silicon`<br>`curve.color_ramp_oklab_accelerated_x86_64` | 已实现 |
| [CRV-06F1 — color_ramp_oklch](op_specs/CRV-06F1_color_ramp_oklch.md) | primitive / D1_draft | `curve.color_ramp_oklch_strict`<br>`curve.color_ramp_oklch_accelerated_apple_silicon`<br>`curve.color_ramp_oklch_accelerated_x86_64` | 已实现 |
| [CRV-06F2 — color_ramp_oklch_pi](op_specs/CRV-06F2_color_ramp_oklch_pi.md) | primitive / D1_draft | `curve.color_ramp_oklch_pi_strict`<br>`curve.color_ramp_oklch_pi_accelerated_apple_silicon`<br>`curve.color_ramp_oklch_pi_accelerated_x86_64` | 已实现 |
| [CRV-06F3 — color_ramp_oklch_rational_pi](op_specs/CRV-06F3_color_ramp_oklch_rational_pi.md) | primitive / D1_draft | `curve.color_ramp_oklch_rational_pi_strict`<br>`curve.color_ramp_oklch_rational_pi_accelerated_apple_silicon`<br>`curve.color_ramp_oklch_rational_pi_accelerated_x86_64` | 已实现 |
| [CRV-06G1 — color_ramp_hsl](op_specs/CRV-06G1_color_ramp_hsl.md) | primitive / D1_draft | `curve.color_ramp_hsl_strict`<br>`curve.color_ramp_hsl_accelerated_apple_silicon`<br>`curve.color_ramp_hsl_accelerated_x86_64` | 已实现 |
| [CRV-06G2 — color_ramp_hsl_pi](op_specs/CRV-06G2_color_ramp_hsl_pi.md) | primitive / D1_draft | `curve.color_ramp_hsl_pi_strict`<br>`curve.color_ramp_hsl_pi_accelerated_apple_silicon`<br>`curve.color_ramp_hsl_pi_accelerated_x86_64` | 已实现 |
| [CRV-06G3 — color_ramp_hsl_rational_pi](op_specs/CRV-06G3_color_ramp_hsl_rational_pi.md) | primitive / D1_draft | `curve.color_ramp_hsl_rational_pi_strict`<br>`curve.color_ramp_hsl_rational_pi_accelerated_apple_silicon`<br>`curve.color_ramp_hsl_rational_pi_accelerated_x86_64` | 已实现 |
| [CRV-06H — color_ramp_ycbcr](op_specs/CRV-06H_color_ramp_ycbcr.md) | primitive / D1_draft | `curve.color_ramp_ycbcr_strict`<br>`curve.color_ramp_ycbcr_accelerated_apple_silicon`<br>`curve.color_ramp_ycbcr_accelerated_x86_64` | 已实现 |
| [CRV-06I — color_ramp_xyz](op_specs/CRV-06I_color_ramp_xyz.md) | primitive / D1_draft | `curve.color_ramp_xyz_strict`<br>`curve.color_ramp_xyz_accelerated_apple_silicon`<br>`curve.color_ramp_xyz_accelerated_x86_64` | 已实现 |
| [CRV-07A — apply_lut3d_trilinear](op_specs/CRV-07A_apply_lut3d_trilinear.md) | primitive / D1_draft | `curve.apply_lut3d_trilinear_strict`<br>`curve.apply_lut3d_trilinear_accelerated_apple_silicon`<br>`curve.apply_lut3d_trilinear_accelerated_x86_64` | 已实现 |
| [CRV-07B — apply_lut3d_tetrahedral](op_specs/CRV-07B_apply_lut3d_tetrahedral.md) | primitive / D1_draft | `curve.apply_lut3d_tetrahedral_strict`<br>`curve.apply_lut3d_tetrahedral_accelerated_apple_silicon`<br>`curve.apply_lut3d_tetrahedral_accelerated_x86_64` | 已实现 |
| [CRV-08A — linear_shaper](op_specs/CRV-08A_linear_shaper.md) | composite_workflow / D1_draft | — | 已实现 |
| [CRV-08B — linear_shaper_inverse](op_specs/CRV-08B_linear_shaper_inverse.md) | composite_workflow / D1_draft | — | 已实现 |
| [CRV-08C — log2_shaper](op_specs/CRV-08C_log2_shaper.md) | primitive / D1_draft | `curve.log2_shaper_strict`<br>`curve.log2_shaper_accelerated_apple_silicon`<br>`curve.log2_shaper_accelerated_x86_64` | 已实现 |
| [CRV-08D — log2_shaper_inverse](op_specs/CRV-08D_log2_shaper_inverse.md) | primitive / D1_draft | `curve.log2_shaper_inverse_strict`<br>`curve.log2_shaper_inverse_accelerated_apple_silicon`<br>`curve.log2_shaper_inverse_accelerated_x86_64` | 已实现 |
| [CRV-09 — bake_lut3d workflow template](op_specs/CRV-09_bake_lut3d.md) | composite_workflow / D1_draft | — | 已实现 |
| [CRV-10A — invert_linear](op_specs/CRV-10A_invert_linear.md) | primitive / D1_draft | `curve.invert_linear_strict`<br>`curve.invert_linear_accelerated_apple_silicon`<br>`curve.invert_linear_accelerated_x86_64` | 已实现 |
| [CRV-10B — invert_pchip](op_specs/CRV-10B_invert_pchip.md) | primitive / D1_draft | `curve.invert_pchip_strict`<br>`curve.invert_pchip_accelerated_apple_silicon`<br>`curve.invert_pchip_accelerated_x86_64` | 已实现 |
| [CRV-11A — resample_linear](op_specs/CRV-11A_resample_linear.md) | composite_workflow / D1_draft | — | 已实现 |
| [CRV-11B — resample_pchip](op_specs/CRV-11B_resample_pchip.md) | composite_workflow / D1_draft | — | 已实现 |
| [CRV-11C — resample_linear_multi](op_specs/CRV-11C_resample_linear_multi.md) | composite_workflow / D1_draft | — | 已实现 |
| [CRV-11D — resample_pchip_multi](op_specs/CRV-11D_resample_pchip_multi.md) | composite_workflow / D1_draft | — | 已实现 |
| [CRV-11E1 — lowpass_uniform_hann_sinc](op_specs/CRV-11E1_lowpass_uniform_hann_sinc.md) | primitive / D1_draft | `curve.lowpass_uniform_hann_sinc_strict`<br>`curve.lowpass_uniform_hann_sinc_accelerated_apple_silicon`<br>`curve.lowpass_uniform_hann_sinc_accelerated_x86_64` | 已实现 |
| [CRV-11E2 — lowpass_uniform_hamming_sinc](op_specs/CRV-11E2_lowpass_uniform_hamming_sinc.md) | primitive / D1_draft | `curve.lowpass_uniform_hamming_sinc_strict`<br>`curve.lowpass_uniform_hamming_sinc_accelerated_apple_silicon`<br>`curve.lowpass_uniform_hamming_sinc_accelerated_x86_64` | 已实现 |
| [CRV-11E3 — lowpass_uniform_blackman_sinc](op_specs/CRV-11E3_lowpass_uniform_blackman_sinc.md) | primitive / D1_draft | `curve.lowpass_uniform_blackman_sinc_strict`<br>`curve.lowpass_uniform_blackman_sinc_accelerated_apple_silicon`<br>`curve.lowpass_uniform_blackman_sinc_accelerated_x86_64` | 已实现 |
| [CRV-11E4 — lowpass_uniform_kaiser_sinc](op_specs/CRV-11E4_lowpass_uniform_kaiser_sinc.md) | primitive / D1_draft | `curve.lowpass_uniform_kaiser_sinc_strict`<br>`curve.lowpass_uniform_kaiser_sinc_accelerated_apple_silicon`<br>`curve.lowpass_uniform_kaiser_sinc_accelerated_x86_64` | 已实现 |
| [CRV-11E5 — lowpass_uniform_gaussian](op_specs/CRV-11E5_lowpass_uniform_gaussian.md) | primitive / D1_draft | `curve.lowpass_uniform_gaussian_strict`<br>`curve.lowpass_uniform_gaussian_accelerated_apple_silicon`<br>`curve.lowpass_uniform_gaussian_accelerated_x86_64` | 已实现 |
| [CRV-11F1 — lowpass_nonuniform_hann_sinc](op_specs/CRV-11F1_lowpass_nonuniform_hann_sinc.md) | primitive / D1_draft | `curve.lowpass_nonuniform_hann_sinc_strict`<br>`curve.lowpass_nonuniform_hann_sinc_accelerated_apple_silicon`<br>`curve.lowpass_nonuniform_hann_sinc_accelerated_x86_64` | 已实现 |
| [CRV-11F2 — lowpass_nonuniform_hamming_sinc](op_specs/CRV-11F2_lowpass_nonuniform_hamming_sinc.md) | primitive / D1_draft | `curve.lowpass_nonuniform_hamming_sinc_strict`<br>`curve.lowpass_nonuniform_hamming_sinc_accelerated_apple_silicon`<br>`curve.lowpass_nonuniform_hamming_sinc_accelerated_x86_64` | 已实现 |
| [CRV-11F3 — lowpass_nonuniform_blackman_sinc](op_specs/CRV-11F3_lowpass_nonuniform_blackman_sinc.md) | primitive / D1_draft | `curve.lowpass_nonuniform_blackman_sinc_strict`<br>`curve.lowpass_nonuniform_blackman_sinc_accelerated_apple_silicon`<br>`curve.lowpass_nonuniform_blackman_sinc_accelerated_x86_64` | 已实现 |
| [CRV-11F4 — lowpass_nonuniform_kaiser_sinc](op_specs/CRV-11F4_lowpass_nonuniform_kaiser_sinc.md) | primitive / D1_draft | `curve.lowpass_nonuniform_kaiser_sinc_strict`<br>`curve.lowpass_nonuniform_kaiser_sinc_accelerated_apple_silicon`<br>`curve.lowpass_nonuniform_kaiser_sinc_accelerated_x86_64` | 已实现 |
| [CRV-11F5 — lowpass_nonuniform_gaussian](op_specs/CRV-11F5_lowpass_nonuniform_gaussian.md) | primitive / D1_draft | `curve.lowpass_nonuniform_gaussian_strict`<br>`curve.lowpass_nonuniform_gaussian_accelerated_apple_silicon`<br>`curve.lowpass_nonuniform_gaussian_accelerated_x86_64` | 已实现 |

## 相关入口

- [数值与数组基础](core.md)
- [公共数据约定](../00-foundation/contracts.md)
- [数学 oracle](../../../oracle/ops/numeric/README.md)

## 来源

[^clf]: Academy，[*CLF Specification*](https://docs.acescentral.com/clf/specification/)，CLF v3；[Implementation Guide](https://docs.acescentral.com/clf/guides/)，LUT形状/顺序和独立测试资料。
[^pchip]: SciPy，[*PchipInterpolator*](https://docs.scipy.org/doc/scipy/reference/generated/scipy.interpolate.PchipInterpolator.html)，访问页面v1.18.0；单调插值与节点约束。
[^bspline]: SciPy，[*BSpline*](https://docs.scipy.org/doc/scipy/reference/generated/scipy.interpolate.BSpline.html)，访问页面v1.18.0；degree/knots。
[^resample]: SciPy，[*resample*](https://docs.scipy.org/doc/scipy/reference/generated/scipy.signal.resample.html)，访问页面v1.18.0；Fourier周期假设。
