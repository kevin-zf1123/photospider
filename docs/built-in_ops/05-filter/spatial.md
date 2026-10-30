# 空间滤波、边缘与细节

本页是 FIL-01～20 的入口目录。端口、参数、公式、数值类别与边界行为以链接的英文族契约和成员规格为准；频域、去噪与恢复族见[频域、去噪与恢复](frequency-restoration.md)。

## 基本术语

| 术语 | 含义 |
| --- | --- |
| Raw field | 无颜色解释的数值 Tensor；通道数不暗示 RGB 或 alpha。 |
| 语义颜色图像 | 由显式静态颜色参数解释的图像，沿用 FMT straight-color 语义；alpha 不作为普通颜色通道滤波。 |
| Anchor | 核的原点样本。偶数尺寸核的 anchor 由成员参数显式给出。 |
| 边界扩展 | 作用于完整逻辑图像的 constant、clamp、reflect_half、reflect_whole、wrap 等规则；ROI 与 tile 边缘不构成新边界。 |
| 参与 mask / 应用 mask | 参与 mask 决定哪些样本进入计算，应用 mask 决定结果写回哪些位置，两者不能互换。 |
| sample shape | 包含 batch 前缀的完整 Tensor shape；`x_axis`、`y_axis` 按完整 shape 索引，例如 RGBA `{N,L,H,W,4}` 使用 `y_axis=2`、`x_axis=3`。 |

## 基础数值规定

数值类别、舍入阶段与浮点特殊值见[数值参考](op_specs/FILTER_numeric_reference.md)；继承关系、表示与 unchecked 调用方前提见[共同契约](op_specs/FILTER_common_contract.md)；边界与 anchor 见[边界契约](op_specs/FILTER_boundary_contract.md)；正核颜色模糊的 associate/filter/unassociate 舍入链见[颜色组合](op_specs/FILTER_color_composition.md)；Bands 等多 Tensor 结构见[集合契约](op_specs/FILTER_collections_contract.md)。

主要语义区别：

- FIL-01A 卷积与 FIL-01B 相关的核坐标方向相反；full/valid 输出的全局原点由成员规定。
- FIL-01D 是正核语义颜色模糊的融合 primitive，输出保持 straight alpha，舍入链与 raw-field 卷积不同。
- Gaussian 使用规定的 baked64 系数，不依赖库的中间 dtype 默认。
- 可分离滤波的两遍计算保留同一精确表达式，不引入未声明的中间舍入。
- 导向滤波由两层局部均值构成，最坏支持半径为 2r；Canny 滞后连通依赖整个平面。
- 金字塔各层保留奇偶尺寸与采样相位；Local Laplacian 与直接细节增益是不同算子。
- FIL-19A 是自定义方向后处理抗锯齿，不等同于 FXAA 或 SMAA；FIL-19B 使用固定的 `smaa_1x_low_luma_v1` profile。

## 族目录

| 族 | 族契约 | 成员 |
| --- | --- | --- |
| FIL-01 | [一般二维核](op_specs/FIL-01_contract.md) | [FIL-01A](op_specs/FIL-01A_convolve2d.md)、[FIL-01B](op_specs/FIL-01B_correlate2d.md)、[FIL-01C](op_specs/FIL-01C_normalized_convolution.md)、[FIL-01D](op_specs/FIL-01D_positive_color_blur.md) |
| FIL-02 | [可分离核](op_specs/FIL-02_contract.md) | [FIL-02A](op_specs/FIL-02A_separable_convolution.md)、[FIL-02B](op_specs/FIL-02B_separable_correlation.md) |
| FIL-03 | [盒式统计](op_specs/FIL-03_contract.md) | [FIL-03A](op_specs/FIL-03A_box_sum.md)、[FIL-03B](op_specs/FIL-03B_box_mean.md) |
| FIL-04 | [有限离散 Gaussian](op_specs/FIL-04_contract.md) | [FIL-04A](op_specs/FIL-04A_gaussian_coefficients.md)、[FIL-04B](op_specs/FIL-04B_gaussian_filter.md) |
| FIL-05 | [秩滤波](op_specs/FIL-05_contract.md) | [FIL-05A](op_specs/FIL-05A_median.md)、[FIL-05B](op_specs/FIL-05B_percentile.md) |
| FIL-06 | [双边与联合双边](op_specs/FIL-06_contract.md) | [FIL-06A](op_specs/FIL-06A_bilateral.md)、[FIL-06B](op_specs/FIL-06B_joint_bilateral.md) |
| FIL-07 | [导向滤波](op_specs/FIL-07_contract.md) | [FIL-07A](op_specs/FIL-07A_guided_scalar.md)、[FIL-07B](op_specs/FIL-07B_guided_vector.md) |
| FIL-08 | [一阶导数](op_specs/FIL-08_contract.md) | [FIL-08A](op_specs/FIL-08A_central_gradient.md)、[FIL-08B](op_specs/FIL-08B_sobel_gradient.md)、[FIL-08C](op_specs/FIL-08C_scharr_gradient.md) |
| FIL-09 | [梯度派生量](op_specs/FIL-09_contract.md) | [FIL-09A](op_specs/FIL-09A_gradient_magnitude.md)、[FIL-09B](op_specs/FIL-09B_gradient_orientation.md) |
| FIL-10 | [二阶 Laplacian](op_specs/FIL-10_contract.md) | [FIL-10A](op_specs/FIL-10A_laplacian4.md)、[FIL-10B](op_specs/FIL-10B_laplacian8_isotropic.md) |
| FIL-11 | [LoG 与 DoG](op_specs/FIL-11_contract.md) | [FIL-11A](op_specs/FIL-11A_log_kernel.md)、[FIL-11B](op_specs/FIL-11B_log_filter.md)、[FIL-11C](op_specs/FIL-11C_difference_of_gaussians.md) |
| FIL-12 | [局部二阶结构](op_specs/FIL-12_contract.md) | [FIL-12A](op_specs/FIL-12A_hessian2d.md)、[FIL-12B](op_specs/FIL-12B_structure_tensor2d.md)、[FIL-12C](op_specs/FIL-12C_symmetric_eigen2d.md) |
| FIL-13 | [Canny 与滞后连通](op_specs/FIL-13_contract.md) | [FIL-13A](op_specs/FIL-13A_canny_quantized4.md)、[FIL-13B](op_specs/FIL-13B_hysteresis_edges.md) |
| FIL-14 | [Gabor 与滤波器组](op_specs/FIL-14_contract.md) | [FIL-14A](op_specs/FIL-14A_gabor_kernel.md)、[FIL-14B](op_specs/FIL-14B_gabor_response.md)、[FIL-14C](op_specs/FIL-14C_gabor_bank.md) |
| FIL-15 | [Gaussian/Laplacian 金字塔](op_specs/FIL-15_contract.md) | [FIL-15A](op_specs/FIL-15A_gaussian_pyramid.md)、[FIL-15B](op_specs/FIL-15B_laplacian_pyramid.md)、[FIL-15C](op_specs/FIL-15C_reconstruct_laplacian.md) |
| FIL-16 | [锐化与残差增益](op_specs/FIL-16_contract.md) | [FIL-16A](op_specs/FIL-16A_unsharp_mask.md)、[FIL-16B](op_specs/FIL-16B_detail_gain.md) |
| FIL-17 | [局部对比与 Local Laplacian](op_specs/FIL-17_contract.md) | [FIL-17A](op_specs/FIL-17A_local_contrast_tanh.md)、[FIL-17B](op_specs/FIL-17B_local_laplacian_reference.md) |
| FIL-18 | [变半径 gather](op_specs/FIL-18_contract.md) | [FIL-18A](op_specs/FIL-18A_variable_box_gather.md)、[FIL-18B](op_specs/FIL-18B_variable_disk_gather.md) |
| FIL-19 | [静态后处理抗锯齿](op_specs/FIL-19_contract.md) | [FIL-19A](op_specs/FIL-19A_directional_post_aa_v1.md)、[FIL-19B](op_specs/FIL-19B_smaa_1x_native.md) |
| FIL-20 | [局部矩与协方差](op_specs/FIL-20_contract.md) | [FIL-20A](op_specs/FIL-20A_local_mean_variance.md)、[FIL-20B](op_specs/FIL-20B_local_covariance.md) |

## 逐成员入口

| ID / spec | 类别 | 注册 key | 实现 |
| --- | --- | --- | --- |
| [FIL-01A — convolve2d](op_specs/FIL-01A_convolve2d.md) | primitive / D1_draft | `filter.convolve2d_strict`<br>`filter.convolve2d_accelerated_apple_silicon`<br>`filter.convolve2d_accelerated_x86_64` | 已实现 |
| [FIL-01B — correlate2d](op_specs/FIL-01B_correlate2d.md) | primitive / D1_draft | `filter.correlate2d_strict`<br>`filter.correlate2d_accelerated_apple_silicon`<br>`filter.correlate2d_accelerated_x86_64` | 已实现 |
| [FIL-01C — normalized_convolution](op_specs/FIL-01C_normalized_convolution.md) | primitive / D1_draft | `filter.normalized_convolution_strict`<br>`filter.normalized_convolution_accelerated_apple_silicon`<br>`filter.normalized_convolution_accelerated_x86_64` | 已实现 |
| [FIL-01D — positive_color_blur](op_specs/FIL-01D_positive_color_blur.md) | primitive / D1_draft | `filter.positive_color_blur_strict` | 已实现 |
| [FIL-02A — separable_convolution](op_specs/FIL-02A_separable_convolution.md) | primitive / D1_draft | `filter.separable_convolution_strict`<br>`filter.separable_convolution_accelerated_apple_silicon`<br>`filter.separable_convolution_accelerated_x86_64` | 已实现 |
| [FIL-02B — separable_correlation](op_specs/FIL-02B_separable_correlation.md) | primitive / D1_draft | `filter.separable_correlation_strict`<br>`filter.separable_correlation_accelerated_apple_silicon`<br>`filter.separable_correlation_accelerated_x86_64` | 已实现 |
| [FIL-03A — box_sum](op_specs/FIL-03A_box_sum.md) | primitive / D1_draft | `filter.box_sum_strict`<br>`filter.box_sum_accelerated_apple_silicon`<br>`filter.box_sum_accelerated_x86_64` | 已实现 |
| [FIL-03B — box_mean](op_specs/FIL-03B_box_mean.md) | primitive / D1_draft | `filter.box_mean_strict`<br>`filter.box_mean_accelerated_apple_silicon`<br>`filter.box_mean_accelerated_x86_64` | 已实现 |
| [FIL-04A — gaussian_coefficients](op_specs/FIL-04A_gaussian_coefficients.md) | primitive / D1_draft | `filter.gaussian_coefficients_strict`<br>`filter.gaussian_coefficients_accelerated_apple_silicon`<br>`filter.gaussian_coefficients_accelerated_x86_64` | 已实现 |
| [FIL-04B — gaussian_filter](op_specs/FIL-04B_gaussian_filter.md) | primitive / D1_draft | `filter.gaussian_filter_strict`<br>`filter.gaussian_filter_accelerated_apple_silicon`<br>`filter.gaussian_filter_accelerated_x86_64` | 已实现 |
| [FIL-05A — median](op_specs/FIL-05A_median.md) | primitive / D1_draft | `filter.median_strict`<br>`filter.median_accelerated_apple_silicon`<br>`filter.median_accelerated_x86_64` | 已实现 |
| [FIL-05B — percentile](op_specs/FIL-05B_percentile.md) | primitive / D1_draft | `filter.percentile_strict`<br>`filter.percentile_accelerated_apple_silicon`<br>`filter.percentile_accelerated_x86_64` | 已实现 |
| [FIL-06A — bilateral](op_specs/FIL-06A_bilateral.md) | primitive / D2_draft | `filter.bilateral_strict`<br>`filter.bilateral_accelerated_apple_silicon`<br>`filter.bilateral_accelerated_x86_64` | 已实现 |
| [FIL-06B — joint_bilateral](op_specs/FIL-06B_joint_bilateral.md) | primitive / D2_draft | `filter.joint_bilateral_strict`<br>`filter.joint_bilateral_accelerated_apple_silicon`<br>`filter.joint_bilateral_accelerated_x86_64` | 已实现 |
| [FIL-07A — guided_scalar](op_specs/FIL-07A_guided_scalar.md) | primitive / D2_draft | `filter.guided_scalar_strict`<br>`filter.guided_scalar_accelerated_apple_silicon`<br>`filter.guided_scalar_accelerated_x86_64` | 已实现 |
| [FIL-07B — guided_vector](op_specs/FIL-07B_guided_vector.md) | primitive / D2_draft | `filter.guided_vector_strict`<br>`filter.guided_vector_accelerated_apple_silicon`<br>`filter.guided_vector_accelerated_x86_64` | 已实现 |
| [FIL-08A — central_gradient](op_specs/FIL-08A_central_gradient.md) | primitive / D1_draft | `filter.central_gradient_strict`<br>`filter.central_gradient_accelerated_apple_silicon`<br>`filter.central_gradient_accelerated_x86_64` | 已实现 |
| [FIL-08B — sobel_gradient](op_specs/FIL-08B_sobel_gradient.md) | primitive / D1_draft | `filter.sobel_gradient_strict`<br>`filter.sobel_gradient_accelerated_apple_silicon`<br>`filter.sobel_gradient_accelerated_x86_64` | 已实现 |
| [FIL-08C — scharr_gradient](op_specs/FIL-08C_scharr_gradient.md) | primitive / D1_draft | `filter.scharr_gradient_strict`<br>`filter.scharr_gradient_accelerated_apple_silicon`<br>`filter.scharr_gradient_accelerated_x86_64` | 已实现 |
| [FIL-09A — gradient_magnitude](op_specs/FIL-09A_gradient_magnitude.md) | primitive / D1_draft | `filter.gradient_magnitude_strict`<br>`filter.gradient_magnitude_accelerated_apple_silicon`<br>`filter.gradient_magnitude_accelerated_x86_64` | 已实现 |
| [FIL-09B — gradient_orientation](op_specs/FIL-09B_gradient_orientation.md) | primitive / D1_draft | `filter.gradient_orientation_strict`<br>`filter.gradient_orientation_accelerated_apple_silicon`<br>`filter.gradient_orientation_accelerated_x86_64` | 已实现 |
| [FIL-10A — laplacian4](op_specs/FIL-10A_laplacian4.md) | primitive / D1_draft | `filter.laplacian4_strict`<br>`filter.laplacian4_accelerated_apple_silicon`<br>`filter.laplacian4_accelerated_x86_64` | 已实现 |
| [FIL-10B — laplacian8_isotropic](op_specs/FIL-10B_laplacian8_isotropic.md) | primitive / D1_draft | `filter.laplacian8_isotropic_strict`<br>`filter.laplacian8_isotropic_accelerated_apple_silicon`<br>`filter.laplacian8_isotropic_accelerated_x86_64` | 已实现 |
| [FIL-11A — log_kernel](op_specs/FIL-11A_log_kernel.md) | primitive / D1_draft | `filter.log_kernel_strict`<br>`filter.log_kernel_accelerated_apple_silicon`<br>`filter.log_kernel_accelerated_x86_64` | 已实现 |
| [FIL-11B — log_filter](op_specs/FIL-11B_log_filter.md) | primitive / D1_draft | `filter.log_filter_strict`<br>`filter.log_filter_accelerated_apple_silicon`<br>`filter.log_filter_accelerated_x86_64` | 已实现 |
| [FIL-11C — difference_of_gaussians](op_specs/FIL-11C_difference_of_gaussians.md) | primitive / D1_draft | `filter.difference_of_gaussians_strict`<br>`filter.difference_of_gaussians_accelerated_apple_silicon`<br>`filter.difference_of_gaussians_accelerated_x86_64` | 已实现 |
| [FIL-12A — hessian2d](op_specs/FIL-12A_hessian2d.md) | primitive / D1_draft | `filter.hessian2d_strict`<br>`filter.hessian2d_accelerated_apple_silicon`<br>`filter.hessian2d_accelerated_x86_64` | 已实现 |
| [FIL-12B — structure_tensor2d](op_specs/FIL-12B_structure_tensor2d.md) | primitive / D1_draft | `filter.structure_tensor2d_strict`<br>`filter.structure_tensor2d_accelerated_apple_silicon`<br>`filter.structure_tensor2d_accelerated_x86_64` | 已实现 |
| [FIL-12C — symmetric_eigen2d](op_specs/FIL-12C_symmetric_eigen2d.md) | primitive / D1_draft | `filter.symmetric_eigen2d_strict`<br>`filter.symmetric_eigen2d_accelerated_apple_silicon`<br>`filter.symmetric_eigen2d_accelerated_x86_64` | 已实现 |
| [FIL-13A — canny_quantized4](op_specs/FIL-13A_canny_quantized4.md) | authoring_helper / D2_draft | — | 已实现 |
| [FIL-13B — hysteresis_edges](op_specs/FIL-13B_hysteresis_edges.md) | primitive / D1_draft | `filter.hysteresis_edges_strict` | 已实现 |
| [FIL-14A — gabor_kernel](op_specs/FIL-14A_gabor_kernel.md) | primitive / D1_draft | `filter.gabor_kernel_strict`<br>`filter.gabor_kernel_accelerated_apple_silicon`<br>`filter.gabor_kernel_accelerated_x86_64` | 已实现 |
| [FIL-14B — gabor_response](op_specs/FIL-14B_gabor_response.md) | primitive / D1_draft | `filter.gabor_response_strict`<br>`filter.gabor_response_accelerated_apple_silicon`<br>`filter.gabor_response_accelerated_x86_64` | 已实现 |
| [FIL-14C — gabor_bank](op_specs/FIL-14C_gabor_bank.md) | authoring_helper / D2_draft | — | 已实现 |
| [FIL-15A — gaussian_pyramid](op_specs/FIL-15A_gaussian_pyramid.md) | primitive / D2_draft | `filter.gaussian_pyramid_strict`<br>`filter.gaussian_pyramid_accelerated_apple_silicon`<br>`filter.gaussian_pyramid_accelerated_x86_64` | 已实现 |
| [FIL-15B — laplacian_pyramid](op_specs/FIL-15B_laplacian_pyramid.md) | primitive / D2_draft | `filter.laplacian_pyramid_strict`<br>`filter.laplacian_pyramid_accelerated_apple_silicon`<br>`filter.laplacian_pyramid_accelerated_x86_64` | 已实现 |
| [FIL-15C — reconstruct_laplacian](op_specs/FIL-15C_reconstruct_laplacian.md) | primitive / D2_draft | `filter.reconstruct_laplacian_strict`<br>`filter.reconstruct_laplacian_accelerated_apple_silicon`<br>`filter.reconstruct_laplacian_accelerated_x86_64` | 已实现 |
| [FIL-16A — unsharp_mask](op_specs/FIL-16A_unsharp_mask.md) | primitive / D1_draft | `filter.unsharp_mask_strict`<br>`filter.unsharp_mask_accelerated_apple_silicon`<br>`filter.unsharp_mask_accelerated_x86_64` | 已实现 |
| [FIL-16B — detail_gain](op_specs/FIL-16B_detail_gain.md) | primitive / D1_draft | `filter.detail_gain_strict`<br>`filter.detail_gain_accelerated_apple_silicon`<br>`filter.detail_gain_accelerated_x86_64` | 已实现 |
| [FIL-17A — local_contrast_tanh](op_specs/FIL-17A_local_contrast_tanh.md) | primitive / D2_draft | `filter.local_contrast_tanh_strict`<br>`filter.local_contrast_tanh_accelerated_apple_silicon`<br>`filter.local_contrast_tanh_accelerated_x86_64` | 已实现 |
| [FIL-17B — local_laplacian_reference](op_specs/FIL-17B_local_laplacian_reference.md) | primitive / D2_draft | `filter.local_laplacian_reference_strict`<br>`filter.local_laplacian_reference_accelerated_apple_silicon`<br>`filter.local_laplacian_reference_accelerated_x86_64` | 已实现 |
| [FIL-18A — variable_box_gather](op_specs/FIL-18A_variable_box_gather.md) | primitive / D1_draft | `filter.variable_box_gather_strict`<br>`filter.variable_box_gather_accelerated_apple_silicon`<br>`filter.variable_box_gather_accelerated_x86_64` | 已实现 |
| [FIL-18B — variable_disk_gather](op_specs/FIL-18B_variable_disk_gather.md) | primitive / D1_draft | `filter.variable_disk_gather_strict`<br>`filter.variable_disk_gather_accelerated_apple_silicon`<br>`filter.variable_disk_gather_accelerated_x86_64` | 已实现 |
| [FIL-19A — directional_post_aa_v1](op_specs/FIL-19A_directional_post_aa_v1.md) | primitive / D2_draft | `filter.directional_post_aa_v1_strict`<br>`filter.directional_post_aa_v1_accelerated_apple_silicon`<br>`filter.directional_post_aa_v1_accelerated_x86_64` | 已实现 |
| [FIL-19B — smaa_1x_native](op_specs/FIL-19B_smaa_1x_native.md) | primitive / D2_draft | `filter.smaa_1x_native_strict` | 已实现 |
| [FIL-20A — local_mean_variance](op_specs/FIL-20A_local_mean_variance.md) | primitive / D1_draft | `filter.local_mean_variance_strict`<br>`filter.local_mean_variance_accelerated_apple_silicon`<br>`filter.local_mean_variance_accelerated_x86_64` | 已实现 |
| [FIL-20B — local_covariance](op_specs/FIL-20B_local_covariance.md) | primitive / D1_draft | `filter.local_covariance_strict`<br>`filter.local_covariance_accelerated_apple_silicon`<br>`filter.local_covariance_accelerated_x86_64` | 已实现 |

## 相关入口

- [研究资料](research-sources.md)
- [oracle 覆盖](oracle-coverage.md)
- [数学 oracle](../../../oracle/ops/filter/README.md)
