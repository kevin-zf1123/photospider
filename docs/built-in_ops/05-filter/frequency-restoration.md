# 频域、去噪与恢复

本页是 FRQ-01～10 与 RES-01～14 的入口目录。端口、参数、公式、数值类别与边界行为以链接的英文族契约和成员规格为准；空间滤波族见[空间滤波、边缘与细节](spatial.md)。

## 基本术语

| 术语 | 含义 |
| --- | --- |
| backward / ortho 归一化 | backward 正变换不缩放、逆变换除以 HW；ortho 两个方向均乘 `1/sqrt(HW)`。 |
| 半谱 (RFFT) | 只压缩 x 轴，保存原始宽度的奇偶性；Hermitian 共轭关系由调用方保证。 |
| FrequencyGrid/v1 | 将实部、虚部系数 Result 与静态变换配置配对的 SDK/workflow 表示，配置包括原始空间 shape、变换轴、采样间距、坐标原点与单位、归一化、索引顺序和 packing。 |
| 具名小波 profile | 固定的 basis、提升步骤与边界规则，即 `haar_mean_lifting_v1`、`cdf53_float_lifting_v1` 和 `stationary_haar_mean_v1`；不接受任意 basis 字符串。 |
| 固定 / 早停 profile | 迭代算法的两种成员：固定 profile 执行 `max_iterations` 步，早停 profile 按成员规定的停止判据结束。 |

## 基础数值规定

FRQ-07～10 和 RES-01～12 使用原生 fp32/fp64 浮点契约（数值类别 F）：输入 dtype 决定工作精度，Float64 始终保持 binary64，不承诺正确舍入、固定 ULP 上界或跨算法逐位一致。RES 的规则见 [RES 原生浮点契约](op_specs/RES_native_contract.md)；FRQ-01～06 与 RES-13/14 使用各自规格规定的数值类别。引用 FIL/FRQ 成员的 helper 保留被调用成员的接口与系数契约。图像语义、直通 copy、有限域与 unchecked 调用方前提见[共同契约](op_specs/FILTER_common_contract.md)；Bands、Spectrum 等结构输出见[集合契约](op_specs/FILTER_collections_contract.md)；异步早停见[迭代共享契约](op_specs/FILTER_iterative_contract.md)。

主要语义区别：

- 显式 Hermitian 投影是独立算子 FRQ-02C；IRFFT 按调用方保证的共轭关系重构，不扫描或丢弃虚部。
- FFT、overlap-add 与 overlap-save 卷积的数学结果与对应直接卷积相同，差异限于 F 契约允许的浮点舍入。
- DCT I～IV 的矩阵与 inverse/ortho 权重由 FRQ-09 规定。
- Wiener 使用噪声信号比 NSR，Tikhonov 使用正则项，两者不同；Richardson–Lucy 使用有限算子 A 的真实转置 Aᵀ 与 sensitivity。
- Anscombe 的代数逆与均值域逆是不同成员。
- RES-11A 是低对比区域平滑，RES-11B 是成对块边界平滑，二者不自动识别色带、块网格或真实边缘；RES-12B 是 Gaussian 低通 helper，频率尺度由调用者指定。
- BM3D/CBM3D 使用固定 `haar_opponent_v1` profile：两阶段正交 Haar 滤波与 Kaiser 加权重建；CBM3D 使用固定 opponent 坐标，在第一坐标上匹配，三个坐标分别滤波。参数、边界及平局规则见 [RES-04 族契约](op_specs/RES-04_contract.md)。

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

## 逐成员入口

### 频域

| ID / spec | 类别 | 注册 key | 实现 |
| --- | --- | --- | --- |
| [FRQ-01A — fft2](op_specs/FRQ-01A_fft2.md) | primitive / D1 | `frequency.fft2` | 已实现 |
| [FRQ-01B — ifft2](op_specs/FRQ-01B_ifft2.md) | primitive / D1 | `frequency.ifft2` | 已实现 |
| [FRQ-02A — rfft2](op_specs/FRQ-02A_rfft2.md) | primitive / D1 | `frequency.rfft2` | 已实现 |
| [FRQ-02B — irfft2](op_specs/FRQ-02B_irfft2.md) | primitive / D1 | `frequency.irfft2` | 已实现 |
| [FRQ-02C — project_hermitian](op_specs/FRQ-02C_project_hermitian.md) | primitive / D1 | `frequency.project_hermitian_strict`<br>`frequency.project_hermitian_accelerated_apple_silicon`<br>`frequency.project_hermitian_accelerated_x86_64` | 已实现 |
| [FRQ-03A — fftshift](op_specs/FRQ-03A_fftshift.md) | primitive / D1 | `frequency.fftshift_strict` | 已实现 |
| [FRQ-03B — ifftshift](op_specs/FRQ-03B_ifftshift.md) | primitive / D1 | `frequency.ifftshift_strict` | 已实现 |
| [FRQ-04A — window2d](op_specs/FRQ-04A_window2d.md) | primitive / D1 | `frequency.window2d_strict`<br>`frequency.window2d_accelerated_apple_silicon`<br>`frequency.window2d_accelerated_x86_64` | 已实现 |
| [FRQ-04B — apply_window](op_specs/FRQ-04B_apply_window.md) | primitive / D1 | `frequency.apply_window_strict`<br>`frequency.apply_window_accelerated_apple_silicon`<br>`frequency.apply_window_accelerated_x86_64` | 已实现 |
| [FRQ-04C — window_statistics](op_specs/FRQ-04C_window_statistics.md) | primitive / D1 | `frequency.window_statistics_strict`<br>`frequency.window_statistics_accelerated_apple_silicon`<br>`frequency.window_statistics_accelerated_x86_64` | 已实现 |
| [FRQ-05A — radial_frequency_response](op_specs/FRQ-05A_radial_frequency_response.md) | primitive / D1 | `frequency.radial_frequency_response_strict`<br>`frequency.radial_frequency_response_accelerated_apple_silicon`<br>`frequency.radial_frequency_response_accelerated_x86_64` | 已实现 |
| [FRQ-05B — band_frequency_response](op_specs/FRQ-05B_band_frequency_response.md) | primitive / D1 | `frequency.band_frequency_response_strict`<br>`frequency.band_frequency_response_accelerated_apple_silicon`<br>`frequency.band_frequency_response_accelerated_x86_64` | 已实现 |
| [FRQ-05C — paired_notch_response](op_specs/FRQ-05C_paired_notch_response.md) | primitive / D2 | `frequency.paired_notch_response_strict`<br>`frequency.paired_notch_response_accelerated_apple_silicon`<br>`frequency.paired_notch_response_accelerated_x86_64` | 已实现 |
| [FRQ-06A — frequency_multiply](op_specs/FRQ-06A_frequency_multiply.md) | primitive / D1 | `frequency.frequency_multiply_strict`<br>`frequency.frequency_multiply_accelerated_apple_silicon`<br>`frequency.frequency_multiply_accelerated_x86_64` | 已实现 |
| [FRQ-07A — linear_fft_convolution](op_specs/FRQ-07A_linear_fft_convolution.md) | primitive / D2 | `frequency.linear_fft_convolution` | 已实现 |
| [FRQ-07B — circular_fft_convolution](op_specs/FRQ-07B_circular_fft_convolution.md) | primitive / D1 | `frequency.circular_fft_convolution` | 已实现 |
| [FRQ-08A — overlap_add](op_specs/FRQ-08A_overlap_add.md) | primitive / D2 | `frequency.overlap_add` | 已实现 |
| [FRQ-08B — overlap_save](op_specs/FRQ-08B_overlap_save.md) | primitive / D2 | `frequency.overlap_save` | 已实现 |
| [FRQ-09A — dct2](op_specs/FRQ-09A_dct2.md) | primitive / D1 | `frequency.dct2` | 已实现 |
| [FRQ-09B — idct2](op_specs/FRQ-09B_idct2.md) | primitive / D1 | `frequency.idct2` | 已实现 |
| [FRQ-10A — decompose_haar_mean_lifting_v1](op_specs/FRQ-10A_decompose_haar_mean_lifting_v1.md) | primitive / D2 | `frequency.decompose_haar_mean_lifting_v1` | 已实现 |
| [FRQ-10B — inverse_haar_mean_lifting_v1](op_specs/FRQ-10B_inverse_haar_mean_lifting_v1.md) | primitive / D2 | `frequency.inverse_haar_mean_lifting_v1` | 已实现 |
| [FRQ-10C — decompose_cdf53_float_lifting_v1](op_specs/FRQ-10C_decompose_cdf53_float_lifting_v1.md) | primitive / D2 | `frequency.decompose_cdf53_float_lifting_v1` | 已实现 |
| [FRQ-10D — inverse_cdf53_float_lifting_v1](op_specs/FRQ-10D_inverse_cdf53_float_lifting_v1.md) | primitive / D2 | `frequency.inverse_cdf53_float_lifting_v1` | 已实现 |
| [FRQ-10E — decompose_stationary_haar_mean_v1](op_specs/FRQ-10E_decompose_stationary_haar_mean_v1.md) | primitive / D2 | `frequency.decompose_stationary_haar_mean_v1` | 已实现 |
| [FRQ-10F — inverse_stationary_haar_mean_v1](op_specs/FRQ-10F_inverse_stationary_haar_mean_v1.md) | primitive / D2 | `frequency.inverse_stationary_haar_mean_v1` | 已实现 |

### 恢复

| ID / spec | 类别 | 注册 key | 实现 |
| --- | --- | --- | --- |
| [RES-01A — noise_mad_highpass](op_specs/RES-01A_noise_mad_highpass.md) | primitive / D2_draft | `restoration.noise_mad_highpass` | 已实现 |
| [RES-01B — calibrated_noise_variance](op_specs/RES-01B_calibrated_noise_variance.md) | primitive / D1_draft | `restoration.calibrated_noise_variance` | 已实现 |
| [RES-02A — nlm_plain](op_specs/RES-02A_nlm_plain.md) | primitive / D2_draft | `restoration.nlm_plain` | 已实现 |
| [RES-02B — nlm_noise_corrected](op_specs/RES-02B_nlm_noise_corrected.md) | primitive / D2_draft | `restoration.nlm_noise_corrected` | 已实现 |
| [RES-03A — threshold_wavelet_details](op_specs/RES-03A_threshold_wavelet_details.md) | primitive / D2_draft | `restoration.threshold_wavelet_details` | 已实现 |
| [RES-03B — cycle_spin_wavelet_denoise](op_specs/RES-03B_cycle_spin_wavelet_denoise.md) | authoring_helper / D2_draft | — | 已实现 |
| [RES-04A — bm3d_native](op_specs/RES-04A_bm3d_native.md) | primitive / D2_draft | `restoration.bm3d_native` | 已实现 |
| [RES-04B — cbm3d_native](op_specs/RES-04B_cbm3d_native.md) | primitive / D2_draft | `restoration.cbm3d_native` | 已实现 |
| [RES-05A — Isotropic ROF denoising, fixed steps](op_specs/RES-05A_tv_rof_isotropic.md) | primitive | `restoration.tv_rof_isotropic` | 未实现 |
| [RES-05B — Anisotropic ROF denoising, fixed steps](op_specs/RES-05B_tv_rof_anisotropic.md) | primitive | `restoration.tv_rof_anisotropic` | 未实现 |
| [RES-05C — Isotropic ROF total-variation denoising with asynchronous early stopping](op_specs/RES-05C_tv_rof_isotropic_early_stop.md) | primitive | `restoration.tv_rof_isotropic_early_stop` | 未实现 |
| [RES-05D — Anisotropic ROF total-variation denoising with asynchronous early stopping](op_specs/RES-05D_tv_rof_anisotropic_early_stop.md) | primitive | `restoration.tv_rof_anisotropic_early_stop` | 未实现 |
| [RES-06A — Exponential conductance diffusion, fixed steps](op_specs/RES-06A_diffusion_exponential.md) | primitive | `restoration.diffusion_exponential` | 未实现 |
| [RES-06B — Reciprocal conductance diffusion, fixed steps](op_specs/RES-06B_diffusion_reciprocal.md) | primitive | `restoration.diffusion_reciprocal` | 未实现 |
| [RES-06C — Exponential edge-preserving diffusion with asynchronous early stopping](op_specs/RES-06C_diffusion_exponential_early_stop.md) | primitive | `restoration.diffusion_exponential_early_stop` | 未实现 |
| [RES-06D — Reciprocal edge-preserving diffusion with asynchronous early stopping](op_specs/RES-06D_diffusion_reciprocal_early_stop.md) | primitive | `restoration.diffusion_reciprocal_early_stop` | 未实现 |
| [RES-07A — anscombe_forward](op_specs/RES-07A_anscombe_forward.md) | primitive / D1_draft | `restoration.anscombe_forward` | 未实现 |
| [RES-07B — anscombe_inverse_algebraic](op_specs/RES-07B_anscombe_inverse_algebraic.md) | primitive / D1_draft | `restoration.anscombe_inverse_algebraic` | 未实现 |
| [RES-07C — anscombe_inverse_mean](op_specs/RES-07C_anscombe_inverse_mean.md) | primitive / D2_draft | `restoration.anscombe_inverse_mean` | 未实现 |
| [RES-07D — generalized_anscombe_forward](op_specs/RES-07D_generalized_anscombe_forward.md) | primitive / D1_draft | `restoration.generalized_anscombe_forward` | 未实现 |
| [RES-07E — generalized_anscombe_inverse_algebraic](op_specs/RES-07E_generalized_anscombe_inverse_algebraic.md) | primitive / D1_draft | `restoration.generalized_anscombe_inverse_algebraic` | 未实现 |
| [RES-07F — generalized_anscombe_inverse_mean](op_specs/RES-07F_generalized_anscombe_inverse_mean.md) | primitive / D2_draft | `restoration.generalized_anscombe_inverse_mean` | 未实现 |
| [RES-08A — wiener_nsr](op_specs/RES-08A_wiener_nsr.md) | primitive / D2_draft | `restoration.wiener_nsr` | 未实现 |
| [RES-08B — tikhonov_periodic](op_specs/RES-08B_tikhonov_periodic.md) | primitive / D2_draft | `restoration.tikhonov_periodic` | 未实现 |
| [RES-09A — Richardson–Lucy, Poisson fixed-step profile](op_specs/RES-09A_richardson_lucy_poisson.md) | primitive | `restoration.richardson_lucy_poisson` | 未实现 |
| [RES-09B — Richardson–Lucy, stabilized fixed-step profile](op_specs/RES-09B_richardson_lucy_stabilized.md) | primitive | `restoration.richardson_lucy_stabilized` | 未实现 |
| [RES-09C — Poisson Richardson–Lucy restoration with asynchronous early stopping](op_specs/RES-09C_richardson_lucy_poisson_early_stop.md) | primitive | `restoration.richardson_lucy_poisson_early_stop` | 未实现 |
| [RES-09D — Stabilized Richardson–Lucy restoration with asynchronous early stopping](op_specs/RES-09D_richardson_lucy_stabilized_early_stop.md) | primitive | `restoration.richardson_lucy_stabilized_early_stop` | 未实现 |
| [RES-10A — Blind PSF projected-gradient estimation, fixed steps](op_specs/RES-10A_blind_psf_projected_gradient.md) | primitive | `restoration.blind_psf_projected_gradient` | 未实现 |
| [RES-10B — Blind image/PSF estimation with asynchronous early stopping](op_specs/RES-10B_blind_psf_projected_gradient_early_stop.md) | primitive | `restoration.blind_psf_projected_gradient_early_stop` | 未实现 |
| [RES-11A — low_contrast_smoothing](op_specs/RES-11A_low_contrast_smoothing.md) | primitive / D2_draft | `restoration.low_contrast_smoothing` | 未实现 |
| [RES-11B — pairwise_block_boundary_smoothing](op_specs/RES-11B_pairwise_block_boundary_smoothing.md) | primitive / D2_draft | `restoration.pairwise_block_boundary_smoothing` | 未实现 |
| [RES-12A — paired_notch_denoise](op_specs/RES-12A_paired_notch_denoise.md) | authoring_helper / D2_draft | — | 未实现 |
| [RES-12B — gaussian_lowpass](op_specs/RES-12B_gaussian_lowpass.md) | authoring_helper / D2_draft | — | 未实现 |
| [RES-13A — nonflat_ball_opening](op_specs/RES-13A_nonflat_ball_opening.md) | primitive / D2_draft | `restoration.nonflat_ball_opening_strict`<br>`restoration.nonflat_ball_opening_accelerated_apple_silicon`<br>`restoration.nonflat_ball_opening_accelerated_x86_64` | 未实现 |
| [RES-13B — subtract_background](op_specs/RES-13B_subtract_background.md) | primitive / D1_draft | `restoration.subtract_background_strict`<br>`restoration.subtract_background_accelerated_apple_silicon`<br>`restoration.subtract_background_accelerated_x86_64` | 未实现 |
| [RES-13C — flat_field_correct](op_specs/RES-13C_flat_field_correct.md) | primitive / D1_draft | `restoration.flat_field_correct_strict`<br>`restoration.flat_field_correct_accelerated_apple_silicon`<br>`restoration.flat_field_correct_accelerated_x86_64` | 未实现 |
| [RES-14A — dark_channel_airlight](op_specs/RES-14A_dark_channel_airlight.md) | primitive / D2_draft | `restoration.dark_channel_airlight_strict` | 未实现 |
| [RES-14B — dark_channel_transmission](op_specs/RES-14B_dark_channel_transmission.md) | primitive / D2_draft | `restoration.dark_channel_transmission_strict`<br>`restoration.dark_channel_transmission_accelerated_apple_silicon`<br>`restoration.dark_channel_transmission_accelerated_x86_64` | 未实现 |
| [RES-14C — apply_dehaze](op_specs/RES-14C_apply_dehaze.md) | primitive / D2_draft | `restoration.apply_dehaze_strict`<br>`restoration.apply_dehaze_accelerated_apple_silicon`<br>`restoration.apply_dehaze_accelerated_x86_64` | 未实现 |
| [RES-14D — dark_channel_dehaze](op_specs/RES-14D_dark_channel_dehaze.md) | authoring_helper / D2_draft | — | 未实现 |

## 相关入口

- [研究资料](research-sources.md)
- [oracle 覆盖](oracle-coverage.md)
- [数学 oracle](../../../oracle/ops/filter/README.md)
