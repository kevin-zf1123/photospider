# 空间滤波、边缘与细节

状态：Proposed。旧测试 kernel 不作为规格依据。本文是空间滤波的中文功能目录，说明成员范围、区别和使用边界；具体端口、公式、数值、需求及资源契约以链接的英文族与成员规格为准。旧版 field/image 实现曾经存在，不代表这些规格已经 Accepted 或运行时已注册。

空间滤波族列于本页；其余 24 个频域、去噪与恢复族见[频域、去噪与恢复](frequency-restoration.md)。

## 20 个空间族 / 47 个成员

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

## 重要语义区别

语义图像沿用 FMT straight-color 语义，alpha 不是普通独立颜色通道；raw numerical field 与语义颜色组也须区分。FIL-01A 卷积与 FIL-01B 相关使用不同坐标方向；偶数核 anchor、full/valid 输出全局原点、reflect_half/reflect_whole 边界、参与 mask 与应用 mask 不能互换。FIL-01D 为正核语义颜色模糊提供融合 primitive，输出保持 straight；它与 raw-field 卷积及 FMT 的分阶段 associate/filter/unassociate 有不同舍入链。Gaussian 使用明确的 baked64 系数定义，不依赖库的中间 dtype 默认。

可分离滤波要求两遍实现保留同一精确表达式，不能未经声明引入中间舍入。导向滤波由两层局部均值构成，最坏支持半径为 2r。Canny 滞后连通是全平面问题。金字塔各层保留奇偶尺寸与采样相位。Local Laplacian 不等同于直接细节增益。FIL-19A 是自定义方向后处理，不冒称 FXAA 或 SMAA；FIL-19B 是独立的原生 SMAA 1x 规格目标，第三方对照实现、版本、资源及验收细节仍待确定。

边界与 anchor 规则见[边界契约](op_specs/FILTER_boundary_contract.md)，数值类别与舍入规则见[数值参考](op_specs/FILTER_numeric_reference.md)，颜色模糊边界见[颜色组合](op_specs/FILTER_color_composition.md)，参考程序与验收要求见[oracle 协议](op_specs/FILTER_oracle_protocol.md)。规格提案不代表运行时已注册，也不代表性能或第三方兼容已经验证。

本类别合计 44 族、114 成员，包含 108 个 primitive 和 6 个 authoring helper。
逐成员参考范围见 [oracle 覆盖](oracle-coverage.md)，异步早停规则见
[迭代共享契约](op_specs/FILTER_iterative_contract.md)。
