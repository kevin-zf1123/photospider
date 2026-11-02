# 调色与局部色调

本类别覆盖基础调色、局部明暗、自动统计、颜色选择、抖动与半色调；`op_specs/` 下的英文共享、族与成员规格是权威定义。

## 基本术语与数值规定

- 成员通过独立入口提供 tile 结果；GRD-06C 定义组合执行与固定二进制两个注册版本。E、H(r)、W、S 分别表示逐元素、邻域、全局统计和有序递推的数学依赖；tile 划分不改变统计区域或扫描起点。
- 生产计算使用原生 FP32/FP64。独立 oracle 使用严格舍入，作为实现比较对象；浮点误差与离散结果按[共享契约](op_specs/GRD_common_contract.md)分别比较。
- 颜色解释是显式静态参数；alpha 与 AOV 按成员定义保留。输入数值域沿用[公共 unchecked 契约](../00-foundation/contracts.md)。
- [GRD-05 亮度曲线](op_specs/GRD-05_curves_contract.md)的描述式输入要求相对线性 RGB，接受 `scene`、`display`、`scene_relative`、`display_relative` reference；单位为 `1` 或 `relative`，显式尺度须为 `relative`。
- CIELAB 明度存储为 l=L*/100，OKLab 使用原生 L。线性 Y、l、L 是不同坐标；亮度调整有 RGB 同比缩放、XYZ 只改 Y、CIELAB 只改 l、OKLab 只改 L 四个独立路径。
- 统计默认使用二值选区，完整图像自动调整默认排除 alpha=0；软选区及 alpha 加权显式启用。统计选区与效果混合 mask 独立。
- 效果强度、软 mask 混合与色域映射通过显式算子组合。
- [GRD-06C](op_specs/GRD-06C_gradient_map_channels.md)直接选择输入通道作为 query，在目标模型坐标中分别生成各通道 LUT，输出新图像；色相保留完整浮点 radian 或 pi_multiple 数值。`combined` 与 `compiled` 共享契约；所需新增 fast 入口及复用依赖见该规格。

## 族目录

| 族 | 功能 | 成员 |
| --- | --- | --- |
| [GRD-01 曝光](op_specs/GRD-01_exposure_contract.md) | 统一 EV 与逐像素曝光场 | A, B |
| [GRD-02 增益与 LGG](op_specs/GRD-02_offset_gain_contract.md) | 仿射增益偏移及 Lift/Gamma/Gain | A, B |
| [GRD-03 对比度](op_specs/GRD-03_contrast_contract.md) | 线性支点与 log2 曝光轴对比 | A, B |
| [GRD-04 色阶](op_specs/GRD-04_levels_contract.md) | 有界色阶与 signed 扩展 | A, B |
| [GRD-05 曲线](op_specs/GRD-05_curves_contract.md) | 逐通道插值与四条独立亮度调整路径 | A, B, C, D, E, F |
| [GRD-06 渐变映射](op_specs/GRD-06_gradient_map_contract.md) | RGB 渐变与按独立通道 LUT 生成目标模型图像 | A, B, C |
| [GRD-07 三维 LUT](op_specs/GRD-07_lut3d_contract.md) | 三线性与四面体 RGB 联合映射 | A, B |
| [GRD-08 HSL / HSV](op_specs/GRD-08_hsl_hsv_contract.md) | 原生色相、饱和度及明度／值调整 | A, B |
| [GRD-09 LCh](op_specs/GRD-09_lch_contract.md) | CIELCh 与 OKLCh 调整 | A, B |
| [GRD-10 Lab](op_specs/GRD-10_lab_contract.md) | CIELAB 与 OKLab 原生坐标调整 | A, B |
| [GRD-11 白平衡](op_specs/GRD-11_white_balance_contract.md) | RGB 增益、白点适应与 Kelvin/Tint | A, B, C |
| [GRD-12 黑白](op_specs/GRD-12_monochrome_contract.md) | 艺术权重及三种模型中性灰 | A, B, C, D |
| [GRD-13 色调分离](op_specs/GRD-13_posterize_contract.md) | 等间距级数量化 | A |
| [GRD-14 阈值](op_specs/GRD-14_threshold_contract.md) | 固定、局部均值、Gaussian 与 Sauvola | A, B, C, D |
| [GRD-15 通道混合](op_specs/GRD-15_channel_mixer_contract.md) | 矩阵与偏移及输出通道重构 | A |
| [GRD-16 色彩平衡](op_specs/GRD-16_color_balance_contract.md) | 分区 RGB 与对立色色偏 | A, B |
| [GRD-17 反相](op_specs/GRD-17_invert_contract.md) | 指定区间的数值反射 | A |
| [GRD-18 色调分区](op_specs/GRD-18_tonal_zones_contract.md) | 平滑暗部／中间调／高光权重及调色 | A, B |
| [GRD-19 局部阴影高光](op_specs/GRD-19_shadow_highlight_local_contract.md) | 局部基底明暗与独立细节强度 | A, B, C, D |
| [GRD-20 特征曲线](op_specs/GRD-20_feature_curve_contract.md) | 周期／非周期调制与四条亮度替换路径 | A, B, C, D, E, F, G, H, I, J |
| [GRD-21 饱和度](op_specs/GRD-21_saturation_contract.md) | 线性 RGB、对立色色度与 OKLab vibrance | A, B, C |
| [GRD-22 分离色调与色轮](op_specs/GRD-22_split_tone_contract.md) | 分区染色及分通道曝光偏移 | A, B |
| [GRD-23 选择性颜色](op_specs/GRD-23_selective_color_contract.md) | 彩色／中性色选择与对立色校正 | A, B, C |
| [GRD-24 自动调整](op_specs/GRD-24_auto_adjust_contract.md) | 二值／加权统计、自动曝光与多路径色阶 | A, B, C, D, E, F, G, H, I |
| [GRD-25 直方图与 CLAHE](op_specs/GRD-25_histogram_equalize_contract.md) | 全局均衡与独立逻辑网格局部增强 | A, B, C, D, E, F, G |
| [GRD-26 暗角](op_specs/GRD-26_vignette_contract.md) | 椭圆坐标曝光场与完整应用 | A, B |
| [GRD-27 ASC CDL](op_specs/GRD-27_asc_cdl_contract.md) | 正反向及有无裁剪四种风格 | A, B, C, D |
| [GRD-28 Color Warper](op_specs/GRD-28_color_warper_contract.md) | a/b、色相／色度及色度／明度网格变形 | A, B, C |
| [GRD-29 抖动](op_specs/GRD-29_dither_contract.md) | 等间距级数与指定调色板量化 | A, B, C, D, E, F, G |
| [GRD-30 半色调](op_specs/GRD-30_halftone_contract.md) | 点／线网屏、CMYK 色版及艺术合成 | A, B, C, D |

## 逐成员入口

共 101 个成员，规格状态均为 Proposed；实现状态与规格成熟度分别记录。

| 成员 | 类别 / 执行 / 成熟度 | 提议注册 key | 实现状态 |
| --- | --- | --- | --- |
| [GRD-01A Uniform linear exposure](op_specs/GRD-01A_exposure.md) | primitive / tile / D1_draft | `grade.exposure` | implemented |
| [GRD-01B Exposure from a spatial EV field](op_specs/GRD-01B_exposure_field.md) | primitive / tile / D1_draft | `grade.exposure_field` | implemented |
| [GRD-02A Per-component affine adjustment](op_specs/GRD-02A_offset_gain.md) | primitive / tile / D1_draft | `grade.offset_gain` | implemented |
| [GRD-02B Lift, gamma and gain](op_specs/GRD-02B_lift_gamma_gain.md) | primitive / tile / D1_draft | `grade.lift_gamma_gain` | implemented |
| [GRD-03A Linear contrast around a pivot](op_specs/GRD-03A_contrast_linear.md) | primitive / tile / D1_draft | `grade.contrast_linear` | implemented |
| [GRD-03B Contrast on the exposure axis](op_specs/GRD-03B_contrast_log2.md) | primitive / tile / D1_draft | `grade.contrast_log2` | implemented |
| [GRD-04A Bounded levels](op_specs/GRD-04A_levels_bounded.md) | primitive / tile / D1_draft | `grade.levels_bounded` | implemented |
| [GRD-04B Signed levels](op_specs/GRD-04B_levels_signed.md) | primitive / tile / D1_draft | `grade.levels_signed` | implemented |
| [GRD-05A Component curves with linear interpolation](op_specs/GRD-05A_curves_linear.md) | primitive / tile / D1_draft | `grade.curves_linear` | implemented_cpu_fixed_abi_9 |
| [GRD-05B Component curves with PCHIP interpolation](op_specs/GRD-05B_curves_pchip.md) | primitive / tile / D1_draft | `grade.curves_pchip` | implemented_cpu_fixed_abi_9 |
| [GRD-05C Proportional RGB luminance curve](op_specs/GRD-05C_curves_luminance.md) | primitive / tile / D1_draft | `grade.curves_luminance` | implemented_cpu_fixed_abi_9 |
| [GRD-05D XYZ Y curve](op_specs/GRD-05D_curves_xyz_y.md) | primitive / tile / D1_draft | `grade.curves_xyz_y` | implemented_cpu_fixed_abi_9 |
| [GRD-05E CIELAB lightness curve](op_specs/GRD-05E_curves_cielab_l.md) | primitive / tile / D1_draft | `grade.curves_cielab_l` | implemented_cpu_fixed_abi_9 |
| [GRD-05F OKLab lightness curve](op_specs/GRD-05F_curves_oklab_l.md) | primitive / tile / D1_draft | `grade.curves_oklab_l` | implemented_cpu_fixed_abi_9 |
| [GRD-06A Map a feature through an RGB ramp](op_specs/GRD-06A_gradient_map.md) | primitive / tile / D1_draft | `grade.gradient_map` | not_implemented |
| [GRD-06B Image gradient mapping](op_specs/GRD-06B_gradient_map_image.md) | primitive / tile / D1_draft | `grade.gradient_map_image` | not_implemented |
| [GRD-06C Generate a model-space image through independent channel LUTs](op_specs/GRD-06C_gradient_map_channels.md) | composite_workflow / tile / D1_draft | `grade.gradient_map_channels_combined`<br>`grade.gradient_map_channels_compiled` | not_implemented |
| [GRD-07A Trilinear RGB LUT](op_specs/GRD-07A_lut3d_trilinear.md) | primitive / tile / D1_draft | `grade.lut3d_trilinear` | not_implemented |
| [GRD-07B Tetrahedral RGB LUT](op_specs/GRD-07B_lut3d_tetrahedral.md) | primitive / tile / D1_draft | `grade.lut3d_tetrahedral` | not_implemented |
| [GRD-08A Native HSL adjustment](op_specs/GRD-08A_hsl.md) | primitive / tile / D1_draft | `grade.hsl` | implemented_cpu_fixed_abi_9 |
| [GRD-08B Native HSV adjustment](op_specs/GRD-08B_hsv.md) | primitive / tile / D1_draft | `grade.hsv` | implemented_cpu_fixed_abi_9 |
| [GRD-09A CIELCh adjustment](op_specs/GRD-09A_cielch.md) | primitive / tile / D1_draft | `grade.cielch` | implemented_cpu_fixed_abi_9 |
| [GRD-09B OKLCh adjustment](op_specs/GRD-09B_oklch.md) | primitive / tile / D1_draft | `grade.oklch` | implemented_cpu_fixed_abi_9 |
| [GRD-10A CIELAB affine adjustment](op_specs/GRD-10A_lab.md) | primitive / tile / D1_draft | `grade.lab` | implemented_cpu_fixed_abi_9 |
| [GRD-10B OKLab affine adjustment](op_specs/GRD-10B_oklab.md) | primitive / tile / D1_draft | `grade.oklab` | implemented_cpu_fixed_abi_9 |
| [GRD-11A White balance with RGB gains](op_specs/GRD-11A_white_balance_gains.md) | primitive / tile / D1_draft | `grade.white_balance_gains` | implemented_cpu_fixed_abi_9 |
| [GRD-11B White-point adaptation of RGB](op_specs/GRD-11B_white_balance_adapt.md) | primitive / tile / D1_draft | `grade.white_balance_adapt` | implemented_cpu_fixed_abi_9 |
| [GRD-11C White balance from temperature and tint](op_specs/GRD-11C_white_balance_temperature_tint.md) | primitive / tile / D1_draft | `grade.white_balance_temperature_tint` | implemented_cpu_fixed_abi_9 |
| [GRD-12A Artistic monochrome feature](op_specs/GRD-12A_monochrome_weights.md) | primitive / tile / D1_draft | `grade.monochrome_weights` | implemented_cpu_fixed_abi_9 |
| [GRD-12B Linear-Y monochrome](op_specs/GRD-12B_monochrome_linear_y.md) | primitive / tile / D1_draft | `grade.monochrome_linear_y` | implemented_cpu_fixed_abi_9 |
| [GRD-12C CIELAB monochrome](op_specs/GRD-12C_monochrome_cielab.md) | primitive / tile / D1_draft | `grade.monochrome_cielab` | implemented_cpu_fixed_abi_9 |
| [GRD-12D OKLab monochrome](op_specs/GRD-12D_monochrome_oklab.md) | primitive / tile / D1_draft | `grade.monochrome_oklab` | implemented_cpu_fixed_abi_9 |
| [GRD-13A Uniform posterization](op_specs/GRD-13A_posterize.md) | primitive / tile / D1_draft | `grade.posterize` | implemented_cpu_fixed_abi_9 |
| [GRD-14A Fixed threshold](op_specs/GRD-14A_threshold.md) | primitive / tile / D1_draft | `grade.threshold` | implemented_cpu_fixed_abi_9 |
| [GRD-14B Local-mean threshold](op_specs/GRD-14B_threshold_local_mean.md) | primitive / tile / D1_draft | `grade.threshold_local_mean` | implemented_cpu_fixed_abi_9 |
| [GRD-14C Gaussian-weighted local threshold](op_specs/GRD-14C_threshold_local_gaussian.md) | primitive / tile / D1_draft | `grade.threshold_local_gaussian` | implemented_cpu_fixed_abi_9 |
| [GRD-14D Sauvola local threshold](op_specs/GRD-14D_threshold_sauvola.md) | primitive / tile / D1_draft | `grade.threshold_sauvola` | implemented_cpu_fixed_abi_9 |
| [GRD-15A Matrix channel mixer](op_specs/GRD-15A_channel_mixer.md) | primitive / tile / D1_draft | `grade.channel_mixer` | implemented_cpu_fixed_abi_9 |
| [GRD-16A Tonal RGB color balance](op_specs/GRD-16A_color_balance_rgb.md) | primitive / tile / D1_draft | `grade.color_balance_rgb` | not_implemented |
| [GRD-16B Tonal opponent-color balance](op_specs/GRD-16B_color_balance_opponent.md) | primitive / tile / D1_draft | `grade.color_balance_opponent` | not_implemented |
| [GRD-17A Reflect a numeric range](op_specs/GRD-17A_invert.md) | primitive / tile / D1_draft | `grade.invert` | not_implemented |
| [GRD-18A Soft tonal-zone weights](op_specs/GRD-18A_tonal_zone_masks.md) | primitive / tile / D1_draft | `grade.tonal_zone_masks` | not_implemented |
| [GRD-18B Exposure and offset by tonal zone](op_specs/GRD-18B_tonal_zone_grade.md) | primitive / tile / D1_draft | `grade.tonal_zone_grade` | not_implemented |
| [GRD-19A Bilateral base of log luminance](op_specs/GRD-19A_bilateral_log_base.md) | primitive / tile / D1_draft | `grade.bilateral_log_base` | not_implemented |
| [GRD-19B Adjust a supplied local tonal base](op_specs/GRD-19B_local_base_grade.md) | primitive / tile / D1_draft | `grade.local_base_grade` | not_implemented |
| [GRD-19C Local shadows, highlights and detail](op_specs/GRD-19C_shadow_highlight_local.md) | primitive / tile / D1_draft | `grade.shadow_highlight_local` | not_implemented |
| [GRD-19D Independent local detail strength](op_specs/GRD-19D_local_detail.md) | primitive / tile / D1_draft | `grade.local_detail` | not_implemented |
| [GRD-20A Periodic feature curve](op_specs/GRD-20A_feature_curve_periodic.md) | primitive / tile / D1_draft | `grade.feature_curve_periodic` | not_implemented |
| [GRD-20B Nonperiodic feature curve](op_specs/GRD-20B_feature_curve_linear.md) | primitive / tile / D1_draft | `grade.feature_curve_linear` | not_implemented |
| [GRD-20C Apply a hue-increment field](op_specs/GRD-20C_apply_hue_delta.md) | primitive / tile / D1_draft | `grade.apply_hue_delta` | not_implemented |
| [GRD-20D Apply a chroma-scale field](op_specs/GRD-20D_apply_chroma_scale.md) | primitive / tile / D1_draft | `grade.apply_chroma_scale` | not_implemented |
| [GRD-20E Apply a native lightness-increment field](op_specs/GRD-20E_apply_lightness_delta.md) | primitive / tile / D1_draft | `grade.apply_lightness_delta` | not_implemented |
| [GRD-20F Set luminance by proportional RGB scaling](op_specs/GRD-20F_reconstruct_luminance.md) | primitive / tile / D1_draft | `grade.reconstruct_luminance` | not_implemented |
| [GRD-20G Apply an HSL/HSV saturation-scale field](op_specs/GRD-20G_apply_saturation_scale.md) | primitive / tile / D1_draft | `grade.apply_saturation_scale` | not_implemented |
| [GRD-20H Set XYZ Y while retaining X and Z](op_specs/GRD-20H_set_xyz_y.md) | primitive / tile / D1_draft | `grade.set_xyz_y` | not_implemented |
| [GRD-20I Set CIELAB lightness while retaining a and b](op_specs/GRD-20I_set_cielab_lightness.md) | primitive / tile / D1_draft | `grade.set_cielab_lightness` | not_implemented |
| [GRD-20J Set OKLab lightness while retaining a and b](op_specs/GRD-20J_set_oklab_lightness.md) | primitive / tile / D1_draft | `grade.set_oklab_lightness` | not_implemented |
| [GRD-21A Linear-RGB saturation](op_specs/GRD-21A_saturation_linear.md) | primitive / tile / D1_draft | `grade.saturation_linear` | not_implemented |
| [GRD-21B Opponent-space saturation](op_specs/GRD-21B_saturation_opponent.md) | primitive / tile / D1_draft | `grade.saturation_opponent` | not_implemented |
| [GRD-21C OKLab vibrance](op_specs/GRD-21C_vibrance_oklab.md) | primitive / tile / D1_draft | `grade.vibrance_oklab` | not_implemented |
| [GRD-22A Tonal opponent-color tinting](op_specs/GRD-22A_split_tone_opponent.md) | primitive / tile / D1_draft | `grade.split_tone_opponent` | not_implemented |
| [GRD-22B RGB tonal wheels](op_specs/GRD-22B_color_wheels_rgb.md) | primitive / tile / D1_draft | `grade.color_wheels_rgb` | not_implemented |
| [GRD-23A Soft chromatic-region selection](op_specs/GRD-23A_select_color_region.md) | primitive / tile / D1_draft | `grade.select_color_region` | not_implemented |
| [GRD-23B Adjust selected opponent colors](op_specs/GRD-23B_selective_opponent_adjust.md) | primitive / tile / D1_draft | `grade.selective_opponent_adjust` | not_implemented |
| [GRD-23C Soft neutral-region selection](op_specs/GRD-23C_select_neutral_region.md) | primitive / tile / D1_draft | `grade.select_neutral_region` | not_implemented |
| [GRD-24A Quantiles of a selected feature](op_specs/GRD-24A_selected_quantiles.md) | primitive / tile / D1_draft | `grade.selected_quantiles` | not_implemented |
| [GRD-24B Derive automatic-exposure parameters](op_specs/GRD-24B_auto_exposure_parameters.md) | primitive / tile / D1_draft | `grade.auto_exposure_parameters` | not_implemented |
| [GRD-24C Derive automatic-levels parameters](op_specs/GRD-24C_auto_levels_parameters.md) | primitive / tile / D1_draft | `grade.auto_levels_parameters` | not_implemented |
| [GRD-24D Automatic exposure of a single image](op_specs/GRD-24D_auto_exposure.md) | primitive / tile / D1_draft | `grade.auto_exposure` | not_implemented |
| [GRD-24E Automatic levels in Proportional RGB luminance](op_specs/GRD-24E_auto_levels.md) | primitive / tile / D1_draft | `grade.auto_levels` | not_implemented |
| [GRD-24F Automatic levels in XYZ Y](op_specs/GRD-24F_auto_levels_xyz_y.md) | primitive / tile / D1_draft | `grade.auto_levels_xyz_y` | not_implemented |
| [GRD-24G Automatic levels in CIELAB lightness](op_specs/GRD-24G_auto_levels_cielab_l.md) | primitive / tile / D1_draft | `grade.auto_levels_cielab_l` | not_implemented |
| [GRD-24H Automatic levels in OKLab lightness](op_specs/GRD-24H_auto_levels_oklab_l.md) | primitive / tile / D1_draft | `grade.auto_levels_oklab_l` | not_implemented |
| [GRD-24I Automatic per-channel levels](op_specs/GRD-24I_auto_levels_channels.md) | primitive / tile / D1_draft | `grade.auto_levels_channels` | not_implemented |
| [GRD-25A Selected histogram](op_specs/GRD-25A_histogram.md) | primitive / tile / D1_draft | `grade.histogram` | not_implemented |
| [GRD-25B Histogram CDF mapping](op_specs/GRD-25B_histogram_cdf_map.md) | primitive / tile / D1_draft | `grade.histogram_cdf_map` | not_implemented |
| [GRD-25C Apply a histogram map](op_specs/GRD-25C_histogram_apply.md) | primitive / tile / D1_draft | `grade.histogram_apply` | not_implemented |
| [GRD-25D Generate local CLAHE maps](op_specs/GRD-25D_clahe_maps.md) | primitive / tile / D1_draft | `grade.clahe_maps` | not_implemented |
| [GRD-25E Apply local CLAHE maps](op_specs/GRD-25E_clahe_apply.md) | primitive / tile / D1_draft | `grade.clahe_apply` | not_implemented |
| [GRD-25F Complete histogram equalization](op_specs/GRD-25F_histogram_equalize.md) | primitive / tile / D1_draft | `grade.histogram_equalize` | not_implemented |
| [GRD-25G Complete CLAHE](op_specs/GRD-25G_clahe.md) | primitive / tile / D1_draft | `grade.clahe` | not_implemented |
| [GRD-26A Generate an elliptical vignette EV field](op_specs/GRD-26A_vignette_ev.md) | primitive / tile / D1_draft | `grade.vignette_ev` | not_implemented |
| [GRD-26B Apply an elliptical vignette](op_specs/GRD-26B_vignette.md) | primitive / tile / D1_draft | `grade.vignette` | not_implemented |
| [GRD-27A ASC CDL forward with clamping](op_specs/GRD-27A_asc_cdl_fwd.md) | primitive / tile / D1_draft | `grade.asc_cdl_fwd` | not_implemented |
| [GRD-27B ASC CDL reverse with clamping](op_specs/GRD-27B_asc_cdl_rev.md) | primitive / tile / D1_draft | `grade.asc_cdl_rev` | not_implemented |
| [GRD-27C ASC CDL forward without clamping](op_specs/GRD-27C_asc_cdl_fwd_no_clamp.md) | primitive / tile / D1_draft | `grade.asc_cdl_fwd_no_clamp` | not_implemented |
| [GRD-27D ASC CDL reverse without clamping](op_specs/GRD-27D_asc_cdl_rev_no_clamp.md) | primitive / tile / D1_draft | `grade.asc_cdl_rev_no_clamp` | not_implemented |
| [GRD-28A Opponent-plane color warp](op_specs/GRD-28A_color_warper_ab.md) | primitive / tile / D1_draft | `grade.color_warper_ab` | not_implemented |
| [GRD-28B Periodic hue-chroma warp](op_specs/GRD-28B_color_warper_hc.md) | primitive / tile / D1_draft | `grade.color_warper_hc` | not_implemented |
| [GRD-28C Chroma-lightness warp](op_specs/GRD-28C_color_warper_cl.md) | primitive / tile / D1_draft | `grade.color_warper_cl` | not_implemented |
| [GRD-29A Bayer ordered dither](op_specs/GRD-29A_dither_bayer.md) | primitive / tile / D1_draft | `grade.dither_bayer` | not_implemented |
| [GRD-29B Dither with explicit noise](op_specs/GRD-29B_dither_noise.md) | primitive / tile / D1_draft | `grade.dither_noise` | not_implemented |
| [GRD-29C Floyd-Steinberg uniform-level dither](op_specs/GRD-29C_dither_floyd_steinberg.md) | primitive / tile / D1_draft | `grade.dither_floyd_steinberg` | not_implemented |
| [GRD-29D Dither with an explicit threshold map](op_specs/GRD-29D_dither_ordered_map.md) | primitive / tile / D1_draft | `grade.dither_ordered_map` | not_implemented |
| [GRD-29E Ordered palette dither](op_specs/GRD-29E_dither_palette_ordered.md) | primitive / tile / D1_draft | `grade.dither_palette_ordered` | not_implemented |
| [GRD-29F Palette dither with explicit noise](op_specs/GRD-29F_dither_palette_noise.md) | primitive / tile / D1_draft | `grade.dither_palette_noise` | not_implemented |
| [GRD-29G Floyd-Steinberg palette dither](op_specs/GRD-29G_dither_palette_floyd_steinberg.md) | primitive / tile / D1_draft | `grade.dither_palette_floyd_steinberg` | not_implemented |
| [GRD-30A Circular-dot halftone](op_specs/GRD-30A_halftone_dot.md) | primitive / tile / D1_draft | `grade.halftone_dot` | not_implemented |
| [GRD-30B Line-screen halftone](op_specs/GRD-30B_halftone_line.md) | primitive / tile / D1_draft | `grade.halftone_line` | not_implemented |
| [GRD-30C CMYK separation screens](op_specs/GRD-30C_halftone_cmyk.md) | primitive / tile / D1_draft | `grade.halftone_cmyk` | not_implemented |
| [GRD-30D Artistic color composition of ink masks](op_specs/GRD-30D_halftone_colorize.md) | primitive / tile / D1_draft | `grade.halftone_colorize` | not_implemented |

## 相关入口

- [英文共享契约](op_specs/GRD_common_contract.md)
- [研究来源与数学依据](research-sources.md)
- [严格舍入 oracle](../../../oracle/ops/07-grade/README.md)
- [颜色模型与格式](../02-format-color/representation.md)
- [曲线与 LUT](../01-numeric/curves.md)
- [蒙版与形态学](../04-mask-morphology/masks.md)
