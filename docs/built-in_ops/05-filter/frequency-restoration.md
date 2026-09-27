# 频域、去噪与恢复

状态：Proposed；不沿用旧 FFT/Bands 实现、自动分页策略或 duplicate-last 旧 payload ABI。具名数学 profile 自己规定 shape、stage、边界和 owner；原生 BM3D/CBM3D/SMAA 的数学配置和第三方对照仍须落实，不表示已可运行。

本页列出频域与恢复族；[空间滤波、边缘与细节](spatial.md)列出其余 20 族。

## 10 个频域族 / 26 个成员

| 族 ID | 族契约 | 成员 |
| --- | --- | --- |
| FRQ-01 | [二维复 DFT](op_specs/FRQ-01_contract.md) | [FRQ-01A](op_specs/FRQ-01A_fft2.md)、[FRQ-01B](op_specs/FRQ-01B_ifft2.md) |
| FRQ-02 | [实 DFT 与 Hermitian 网格](op_specs/FRQ-02_contract.md) | [FRQ-02A](op_specs/FRQ-02A_rfft2.md)、[FRQ-02B](op_specs/FRQ-02B_irfft2.md)、[FRQ-02C](op_specs/FRQ-02C_project_hermitian.md) |
| FRQ-03 | [频率重排](op_specs/FRQ-03_contract.md) | [FRQ-03A](op_specs/FRQ-03A_fftshift.md)、[FRQ-03B](op_specs/FRQ-03B_ifftshift.md) |
| FRQ-04 | [窗与标度统计](op_specs/FRQ-04_contract.md) | [FRQ-04A](op_specs/FRQ-04A_window2d.md)、[FRQ-04B](op_specs/FRQ-04B_apply_window.md)、[FRQ-04C](op_specs/FRQ-04C_window_statistics.md) |
| FRQ-05 | [频率响应](op_specs/FRQ-05_contract.md) | [FRQ-05A](op_specs/FRQ-05A_radial_frequency_response.md)、[FRQ-05B](op_specs/FRQ-05B_band_frequency_response.md)、[FRQ-05C](op_specs/FRQ-05C_paired_notch_response.md) |
| FRQ-06 | [复谱乘法](op_specs/FRQ-06_contract.md) | [FRQ-06A](op_specs/FRQ-06A_frequency_multiply.md) |
| FRQ-07 | [线性与循环 FFT 卷积](op_specs/FRQ-07_contract.md) | [FRQ-07A](op_specs/FRQ-07A_linear_fft_convolution.md)、[FRQ-07B](op_specs/FRQ-07B_circular_fft_convolution.md) |
| FRQ-08 | [分块卷积](op_specs/FRQ-08_contract.md) | [FRQ-08A](op_specs/FRQ-08A_overlap_add.md)、[FRQ-08B](op_specs/FRQ-08B_overlap_save.md) |
| FRQ-09 | [DCT I–IV](op_specs/FRQ-09_contract.md) | [FRQ-09A](op_specs/FRQ-09A_dct2.md)、[FRQ-09B](op_specs/FRQ-09B_idct2.md) |
| FRQ-10 | [具名小波分析/重建](op_specs/FRQ-10_contract.md) | [FRQ-10A](op_specs/FRQ-10A_decompose_haar_mean_lifting_v1.md)、[FRQ-10B](op_specs/FRQ-10B_inverse_haar_mean_lifting_v1.md)、[FRQ-10C](op_specs/FRQ-10C_decompose_cdf53_float_lifting_v1.md)、[FRQ-10D](op_specs/FRQ-10D_inverse_cdf53_float_lifting_v1.md)、[FRQ-10E](op_specs/FRQ-10E_decompose_stationary_haar_mean_v1.md)、[FRQ-10F](op_specs/FRQ-10F_inverse_stationary_haar_mean_v1.md) |

## 14 个恢复族 / 41 个成员

| 族 ID | 族契约 | 成员 |
| --- | --- | --- |
| RES-01 | [噪声估计](op_specs/RES-01_contract.md) | [RES-01A](op_specs/RES-01A_noise_mad_highpass.md)、[RES-01B](op_specs/RES-01B_calibrated_noise_variance.md) |
| RES-02 | [Non-local means](op_specs/RES-02_contract.md) | [RES-02A](op_specs/RES-02A_nlm_plain.md)、[RES-02B](op_specs/RES-02B_nlm_noise_corrected.md) |
| RES-03 | [小波去噪](op_specs/RES-03_contract.md) | [RES-03A](op_specs/RES-03A_threshold_wavelet_details.md)、[RES-03B](op_specs/RES-03B_cycle_spin_wavelet_denoise.md) |
| RES-04 | [原生 BM3D/CBM3D](op_specs/RES-04_contract.md) | [RES-04A](op_specs/RES-04A_bm3d_native.md)、[RES-04B](op_specs/RES-04B_cbm3d_native.md) |
| RES-05 | [ROF TV 去噪](op_specs/RES-05_contract.md) | [RES-05A](op_specs/RES-05A_tv_rof_isotropic.md)、[RES-05B](op_specs/RES-05B_tv_rof_anisotropic.md)、[RES-05C](op_specs/RES-05C_tv_rof_isotropic_early_stop.md)、[RES-05D](op_specs/RES-05D_tv_rof_anisotropic_early_stop.md) |
| RES-06 | [各向异性扩散](op_specs/RES-06_contract.md) | [RES-06A](op_specs/RES-06A_diffusion_exponential.md)、[RES-06B](op_specs/RES-06B_diffusion_reciprocal.md)、[RES-06C](op_specs/RES-06C_diffusion_exponential_early_stop.md)、[RES-06D](op_specs/RES-06D_diffusion_reciprocal_early_stop.md) |
| RES-07 | [方差稳定与逆变换](op_specs/RES-07_contract.md) | [RES-07A](op_specs/RES-07A_anscombe_forward.md)、[RES-07B](op_specs/RES-07B_anscombe_inverse_algebraic.md)、[RES-07C](op_specs/RES-07C_anscombe_inverse_mean.md)、[RES-07D](op_specs/RES-07D_generalized_anscombe_forward.md)、[RES-07E](op_specs/RES-07E_generalized_anscombe_inverse_algebraic.md)、[RES-07F](op_specs/RES-07F_generalized_anscombe_inverse_mean.md) |
| RES-08 | [线性频域恢复](op_specs/RES-08_contract.md) | [RES-08A](op_specs/RES-08A_wiener_nsr.md)、[RES-08B](op_specs/RES-08B_tikhonov_periodic.md) |
| RES-09 | [Richardson–Lucy](op_specs/RES-09_contract.md) | [RES-09A](op_specs/RES-09A_richardson_lucy_poisson.md)、[RES-09B](op_specs/RES-09B_richardson_lucy_stabilized.md)、[RES-09C](op_specs/RES-09C_richardson_lucy_poisson_early_stop.md)、[RES-09D](op_specs/RES-09D_richardson_lucy_stabilized_early_stop.md) |
| RES-10 | [盲 PSF 与图像联合估计](op_specs/RES-10_contract.md) | [RES-10A](op_specs/RES-10A_blind_psf_projected_gradient.md)、[RES-10B](op_specs/RES-10B_blind_psf_projected_gradient_early_stop.md) |
| RES-11 | [去色带与去块效应](op_specs/RES-11_contract.md) | [RES-11A](op_specs/RES-11A_low_contrast_smoothing.md)、[RES-11B](op_specs/RES-11B_pairwise_block_boundary_smoothing.md) |
| RES-12 | [去摩尔纹/去网屏](op_specs/RES-12_contract.md) | [RES-12A](op_specs/RES-12A_paired_notch_denoise.md)、[RES-12B](op_specs/RES-12B_gaussian_lowpass.md) |
| RES-13 | [背景与平场校正](op_specs/RES-13_contract.md) | [RES-13A](op_specs/RES-13A_nonflat_ball_opening.md)、[RES-13B](op_specs/RES-13B_subtract_background.md)、[RES-13C](op_specs/RES-13C_flat_field_correct.md) |
| RES-14 | [暗通道去雾](op_specs/RES-14_contract.md) | [RES-14A](op_specs/RES-14A_dark_channel_airlight.md)、[RES-14B](op_specs/RES-14B_dark_channel_transmission.md)、[RES-14C](op_specs/RES-14C_apply_dehaze.md)、[RES-14D](op_specs/RES-14D_dark_channel_dehaze.md) |

RES-11A 现命名为低对比区域平滑，RES-11B 为成对块边界平滑；两者保留原窗口、阈值、mask、strength 混合/配对校正公式。它们不自动识别色带、块网格或真实边缘。RES-12B 命名为 Gaussian low-pass workflow helper，去网屏是用途示例，频率尺度由调用者指定。恢复用途的数学公式合规与逐用途质量评分分开验收；评分指标和阈值在实现时确定。

## 关键选择

DFT backward 正向不缩放、逆向除 HW，ortho 独立；半谱只压缩 x 轴并保存原始奇偶宽。Hermitian 校验/投影分开，不能丢弃虚部假修复。FFT/OLA/OLS 卷积的 strict 数学与直接卷积一致，执行 strategy 不产生新的舍入自由度。DCT I–IV 的矩阵和 inverse/ortho 权重明确；首版小波是三个具名 profile，不接受任意 basis 字符串。

恢复中 Wiener NSR 与 Tikhonov 正则不同；RL 必须使用有限 A 的真实 Aᵀ 和 sensitivity，并明确每步 RN。每种迭代算法同时提供固定 `max_iterations` profile 与独立早停 profile；固定 profile 不暗中早停。Anscombe 代数逆和均值域逆分版；后者两项 oracle 仅诊断级。BM3D/CBM3D 定义为自行原生实现，并须与固定版本的第三方实现对照；当前尚未指定对照实现或版本，不据此宣称已完成对照。

图像语义、直通 copy、有限域和精度详见 [共有契约](op_specs/FILTER_common_contract.md)；结构输出见 [集合契约](op_specs/FILTER_collections_contract.md)。[独立 oracle](../../../oracle/ops/filter/README.md) 不代表 runtime/ISA 验收。

独立参考的逐成员范围见 [oracle 覆盖](oracle-coverage.md)。
