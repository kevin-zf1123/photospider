# Filter oracle coverage

The [oracle README](../../../oracle/ops/filter/README.md) describes reproducible commands
and optional dependencies. [tests.py](../../../oracle/ops/filter/tests.py) owns the
executable assertions and [shared acceptance](op_specs/FILTER_oracle_protocol.md)
defines the separate runtime obligations. This index covers 114 proposed members
in 44 families: 108 primitives and 6 authoring helpers. A listed fixture is not an
all-input proof or an implemented operation.

ExactRational/DirectedMPFR apply only to the expression and finite input subset actually
exercised. ExactSpecial covers targeted IEEE classification, payload and signed-zero
branches. A ContractFixture may be just a manifest/schema assertion. No member has
production runtime, asynchronous stopping, owner, demand, named-backend or quality-score
acceptance from this oracle. Band-lazy runtime carriers and complete nonfinite
restoration trajectories are not implemented. Oracle work caps are not spec size limits.

 Native BM3D/CBM3D/SMAA comparisons need pinned implementations and actual
runs. No synthetic substitute is used as their golden output. Quality metric formulas,
datasets and thresholds remain implementation-time work under the agreed principles.

| Member | Fixture evidence | Representative executable cases | Scope and gaps |
| --- | --- | --- | --- |
| [FIL-01A](op_specs/FIL-01A_convolve2d.md) | ContractFixture, ExactRational, ExactSpecial | `convolution_overflow`, `convolution_negative_zero_float32`, `convolution_negative_denominator_zero_float32`; additional cases in tests.py | Small-array arithmetic/rank fixtures include special values; full payload combinations and runtime remain untested. |
| [FIL-01B](op_specs/FIL-01B_correlate2d.md) | ContractFixture, ExactRational | `boundary_reflect_half`, `boundary_reflect_whole`, `boundary_wrap`; additional cases in tests.py | Small-array arithmetic/rank fixtures include special values; full payload combinations and runtime remain untested. |
| [FIL-01C](op_specs/FIL-01C_normalized_convolution.md) | ExactRational | `normalized_mask_poison_float32`, `normalized_mask_poison_float64` | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [FIL-01D](op_specs/FIL-01D_positive_color_blur.md) | ExactRational, ExactSpecial | `zero_inf_product`, `transparent_finite`, `fused_coverage`; additional cases in tests.py | Single component/window formula including exceptional values; geometry, group metadata and lazy requests untested. |
| [FIL-02A](op_specs/FIL-02A_separable_convolution.md) | ExactRational | `separable_convolve` | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [FIL-02B](op_specs/FIL-02B_separable_correlation.md) | ExactRational | `separable_correlate` | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [FIL-03A](op_specs/FIL-03A_box_sum.md) | ExactRational | `box_False_float32`, `box_False_float64` | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [FIL-03B](op_specs/FIL-03B_box_mean.md) | ExactRational | `box_True_float32`, `box_True_float64` | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [FIL-04A](op_specs/FIL-04A_gaussian_coefficients.md) | ContractFixture, DirectedMPFR | `gaussian_coefficients`, `gaussian_zero_sigma`, `gaussian_zero_sigma_nonzero_radius` | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [FIL-04B](op_specs/FIL-04B_gaussian_filter.md) | DirectedMPFR+ExactRational, ExactRational | `gaussian_constant`, `straight_alpha_zero_hidden_color`, `straight_all_zero_alpha`; additional cases in tests.py | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [FIL-05A](op_specs/FIL-05A_median.md) | ExactRational, ExactSpecial | `median_mean`, `median_even_lower`, `median_even_higher`; additional cases in tests.py | Small-array arithmetic/rank fixtures include special values; full payload combinations and runtime remain untested. |
| [FIL-05B](op_specs/FIL-05B_percentile.md) | ExactRational, ExactSpecial | `rank_opposite_infinities`, `rank_same_infinities`, `percentile_linear_float32`; additional cases in tests.py | Small-array arithmetic/rank fixtures include special values; full payload combinations and runtime remain untested. |
| [FIL-06A](op_specs/FIL-06A_bilateral.md) | DirectedMPFR+ExactRational | `bilateral_symmetric` | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [FIL-06B](op_specs/FIL-06B_joint_bilateral.md) | DirectedMPFR+ExactRational | `joint_bilateral_symmetric` | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [FIL-07A](op_specs/FIL-07A_guided_scalar.md) | ContractFixture, ExactRationalStaged | `guided_scalar_two_windows`, `guided_2r_counterexample` | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [FIL-07B](op_specs/FIL-07B_guided_vector.md) | ExactRationalStaged | `guided_vector_two_windows` | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [FIL-08A](op_specs/FIL-08A_central_gradient.md) | ExactRational | `gradient_ramp_central` | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [FIL-08B](op_specs/FIL-08B_sobel_gradient.md) | ExactRational | `gradient_ramp_sobel` | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [FIL-08C](op_specs/FIL-08C_scharr_gradient.md) | ExactRational | `gradient_ramp_scharr` | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [FIL-09A](op_specs/FIL-09A_gradient_magnitude.md) | DirectedMPFR | `magnitude_3_4` | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [FIL-09B](op_specs/FIL-09B_gradient_orientation.md) | DirectedMPFR | `orientation_turn` | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [FIL-10A](op_specs/FIL-10A_laplacian4.md) | ExactRational | `laplacian_quadratic_False` | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [FIL-10B](op_specs/FIL-10B_laplacian8_isotropic.md) | ExactRational | `laplacian_quadratic_True` | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [FIL-11A](op_specs/FIL-11A_log_kernel.md) | DirectedMPFR | `log_center` | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [FIL-11B](op_specs/FIL-11B_log_filter.md) | DirectedMPFR+ExactRational | `log_constant_zero_dc` | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [FIL-11C](op_specs/FIL-11C_difference_of_gaussians.md) | StagedReference | `dog_constant` | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [FIL-12A](op_specs/FIL-12A_hessian2d.md) | ExactRational | `hessian_polynomial` | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [FIL-12B](op_specs/FIL-12B_structure_tensor2d.md) | ExactRational | `structure_tensor_constant_gradient` | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [FIL-12C](op_specs/FIL-12C_symmetric_eigen2d.md) | DirectedMPFR | `symmetric_eigen` | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [FIL-13A](op_specs/FIL-13A_canny_quantized4.md) | StagedReference | `canny_constant` | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [FIL-13B](op_specs/FIL-13B_hysteresis_edges.md) | ContractFixture, DiscreteExact | `hysteresis_long_chain`, `hysteresis_strong_outside_weak` | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [FIL-14A](op_specs/FIL-14A_gabor_kernel.md) | DirectedMPFR | `gabor_zero_frequency` | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [FIL-14B](op_specs/FIL-14B_gabor_response.md) | ExactRational | `gabor_complex_response` | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [FIL-14C](op_specs/FIL-14C_gabor_bank.md) | StagedReference | `gabor_bank_two_members` | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [FIL-15A](op_specs/FIL-15A_gaussian_pyramid.md) | ExactRationalStaged | `gaussian_pyramid_constant` | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [FIL-15B](op_specs/FIL-15B_laplacian_pyramid.md) | ExactRationalStaged | `laplacian_pyramid_zero_details`, `laplacian_pyramid_odd_roundtrip` | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [FIL-15C](op_specs/FIL-15C_reconstruct_laplacian.md) | ExactRationalStaged | `laplacian_pyramid_odd_roundtrip` | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [FIL-16A](op_specs/FIL-16A_unsharp_mask.md) | BitwiseCopy | `unsharp_disabled_preserves_bits` | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [FIL-16B](op_specs/FIL-16B_detail_gain.md) | ExactRational | `detail_gain_float32`, `detail_gain_float64` | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [FIL-17A](op_specs/FIL-17A_local_contrast_tanh.md) | DirectedMPFR | `local_contrast_equal_base` | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [FIL-17B](op_specs/FIL-17B_local_laplacian_reference.md) | BitwiseCopy, StagedReference | `local_laplacian_identity`, `local_laplacian_actual_remap_constant` | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [FIL-18A](op_specs/FIL-18A_variable_box_gather.md) | ExactRational | `variable_footprint_False` | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [FIL-18B](op_specs/FIL-18B_variable_disk_gather.md) | ExactRational | `variable_footprint_True` | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [FIL-19A](op_specs/FIL-19A_directional_post_aa_v1.md) | BitwiseCopy | `post_aa_flat_guide_copy` | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [FIL-19B](op_specs/FIL-19B_smaa_1x_native.md) | ContractFixture | `manifest_missing_smaa`, `real_golden_smaa` | Manifest structure only; actual native/third-party algorithm comparison pending. |
| [FIL-20A](op_specs/FIL-20A_local_mean_variance.md) | ExactRational | `local_variance_population`, `local_variance_unbiased`, `rational_variance_offset_invariance_0`; additional cases in tests.py | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [FIL-20B](op_specs/FIL-20B_local_covariance.md) | ExactRational | `local_covariance_negative` | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [FRQ-01A](op_specs/FRQ-01A_fft2.md) | Cyclotomic+DirectedMPFR, DirectedMPFR | `dft_normalized_impulse_backward`, `dft_normalized_constant_backward`, `dft_normalized_impulse_ortho`; additional cases in tests.py | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [FRQ-01B](op_specs/FRQ-01B_ifft2.md) | Cyclotomic+DirectedMPFR | `idft_ones_1_1`, `idft_ones_1_5`, `idft_ones_2_3`; additional cases in tests.py | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [FRQ-02A](op_specs/FRQ-02A_rfft2.md) | Cyclotomic+DirectedMPFR | `rfft_odd_width`, `rfft_boundary_column_nonzero_imaginary` | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [FRQ-02B](op_specs/FRQ-02B_irfft2.md) | ContractFixture, Cyclotomic+DirectedMPFR | `irfft_odd_width`, `rfft_boundary_column_nonzero_imaginary`, `irfft_invalid_boundary_conjugacy` | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [FRQ-02C](op_specs/FRQ-02C_project_hermitian.md) | ExactRational+Copy | `hermitian_self_copy_signed_zero` | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [FRQ-03A](op_specs/FRQ-03A_fftshift.md) | BitwiseCopy | `fftshift_odd` | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [FRQ-03B](op_specs/FRQ-03B_ifftshift.md) | BitwiseCopy | `ifftshift_odd` | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [FRQ-04A](op_specs/FRQ-04A_window2d.md) | DirectedMPFR | `hann_periodic`, `hann_symmetric` | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [FRQ-04B](op_specs/FRQ-04B_apply_window.md) | ExactRational | `apply_window` | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [FRQ-04C](op_specs/FRQ-04C_window_statistics.md) | ExactRational | `window_statistics` | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [FRQ-05A](op_specs/FRQ-05A_radial_frequency_response.md) | DirectedMPFR, ExactDiscrete | `ideal_lowpass`, `butterworth_amplitude_cutoff` | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [FRQ-05B](op_specs/FRQ-05B_band_frequency_response.md) | ExactDiscrete | `ideal_bandpass` | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [FRQ-05C](op_specs/FRQ-05C_paired_notch_response.md) | ContractFixture, DirectedMPFR | `paired_notch_zero_at_both_centres`, `duplicate_notch_conjugate_orbit` | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [FRQ-06A](op_specs/FRQ-06A_frequency_multiply.md) | ExactRational | `complex_frequency_multiply` | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [FRQ-07A](op_specs/FRQ-07A_linear_fft_convolution.md) | ContractFixture, ExactRational | `convolution_full`, `convolution_origins` | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [FRQ-07B](op_specs/FRQ-07B_circular_fft_convolution.md) | ExactRational | `circular_kernel_folding` | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [FRQ-08A](op_specs/FRQ-08A_overlap_add.md) | ContractFixture | `block_partition_False` | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [FRQ-08B](op_specs/FRQ-08B_overlap_save.md) | ContractFixture | `block_partition_True` | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [FRQ-09A](op_specs/FRQ-09A_dct2.md) | ContractFixture, Cyclotomic+DirectedMPFR | `dct_I_2x2`, `dct_ortho_single_II_False`, `dct_ortho_single_III_False`; additional cases in tests.py | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [FRQ-09B](op_specs/FRQ-09B_idct2.md) | Cyclotomic+DirectedMPFR | `idct_I_2x2`, `dct_ortho_single_II_True`, `dct_ortho_single_III_True`; additional cases in tests.py | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [FRQ-10A](op_specs/FRQ-10A_decompose_haar_mean_lifting_v1.md) | ExactRationalStaged | `wavelet_roundtrip_haar_mean_lifting_v1`, `wavelet_basis_haar_mean_lifting_v1` | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [FRQ-10B](op_specs/FRQ-10B_inverse_haar_mean_lifting_v1.md) | ExactRationalStaged | `wavelet_roundtrip_haar_mean_lifting_v1` | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [FRQ-10C](op_specs/FRQ-10C_decompose_cdf53_float_lifting_v1.md) | ExactRationalStaged | `wavelet_roundtrip_cdf53_float_lifting_v1`, `wavelet_basis_cdf53_float_lifting_v1` | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [FRQ-10D](op_specs/FRQ-10D_inverse_cdf53_float_lifting_v1.md) | ExactRationalStaged | `wavelet_roundtrip_cdf53_float_lifting_v1` | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [FRQ-10E](op_specs/FRQ-10E_decompose_stationary_haar_mean_v1.md) | ExactRationalStaged | `wavelet_roundtrip_stationary_haar_mean_v1` | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [FRQ-10F](op_specs/FRQ-10F_inverse_stationary_haar_mean_v1.md) | ExactRationalStaged | `wavelet_roundtrip_stationary_haar_mean_v1` | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [RES-01A](op_specs/RES-01A_noise_mad_highpass.md) | DirectedMPFR, ExactRationalStaged | `noise_mad_constant`, `noise_mad_quantile_constant` | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [RES-01B](op_specs/RES-01B_calibrated_noise_variance.md) | ExactRational+DirectedMPFR | `calibrated_variance` | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [RES-02A](op_specs/RES-02A_nlm_plain.md) | DirectedMPFR+ExactRational | `nlm_constant_guide_False` | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [RES-02B](op_specs/RES-02B_nlm_noise_corrected.md) | DirectedMPFR+ExactRational | `nlm_constant_guide_True` | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [RES-03A](op_specs/RES-03A_threshold_wavelet_details.md) | ExactRationalStaged | `wavelet_threshold_soft` | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [RES-03B](op_specs/RES-03B_cycle_spin_wavelet_denoise.md) | ExactRationalStaged | `cycle_spin_constant` | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [RES-04A](op_specs/RES-04A_bm3d_native.md) | ContractFixture | `manifest_missing_bm3d`, `real_golden_bm3d` | Manifest structure only; actual native/third-party algorithm comparison pending. |
| [RES-04B](op_specs/RES-04B_cbm3d_native.md) | ContractFixture | `manifest_missing_cbm3d`, `real_golden_cbm3d` | Manifest structure only; actual native/third-party algorithm comparison pending. |
| [RES-05A](op_specs/RES-05A_tv_rof_isotropic.md) | ExactRationalStaged | `tv_one_step_True` | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [RES-05B](op_specs/RES-05B_tv_rof_anisotropic.md) | ExactRationalStaged | `tv_one_step_False` | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [RES-05C](op_specs/RES-05C_tv_rof_isotropic_early_stop.md) | Not covered in core suite | None | Predicate and asynchronous runtime not implemented in the oracle; fixed-step recurrence fixtures do not validate this profile. |
| [RES-05D](op_specs/RES-05D_tv_rof_anisotropic_early_stop.md) | Not covered in core suite | None | Predicate and asynchronous runtime not implemented in the oracle; fixed-step recurrence fixtures do not validate this profile. |
| [RES-06A](op_specs/RES-06A_diffusion_exponential.md) | DirectedMPFRStaged | `diffusion_exponential_one_step` | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [RES-06B](op_specs/RES-06B_diffusion_reciprocal.md) | ExactRationalStaged | `diffusion_float32_stages`, `diffusion_reciprocal_one_step` | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [RES-06C](op_specs/RES-06C_diffusion_exponential_early_stop.md) | Not covered in core suite | None | Predicate and asynchronous runtime not implemented in the oracle; fixed-step recurrence fixtures do not validate this profile. |
| [RES-06D](op_specs/RES-06D_diffusion_reciprocal_early_stop.md) | Not covered in core suite | None | Predicate and asynchronous runtime not implemented in the oracle; fixed-step recurrence fixtures do not validate this profile. |
| [RES-07A](op_specs/RES-07A_anscombe_forward.md) | DirectedMPFR | `anscombe_forward_zero` | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [RES-07B](op_specs/RES-07B_anscombe_inverse_algebraic.md) | ExactRational | `anscombe_algebraic` | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [RES-07C](op_specs/RES-07C_anscombe_inverse_mean.md) | Not covered in core suite | None | High-precision diagnostic only (optional mpmath); no strict tail/integration/root certificate. |
| [RES-07D](op_specs/RES-07D_generalized_anscombe_forward.md) | DirectedMPFR | `generalized_forward_negative_observation` | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [RES-07E](op_specs/RES-07E_generalized_anscombe_inverse_algebraic.md) | ExactRational | `generalized_algebraic_returns_offset` | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [RES-07F](op_specs/RES-07F_generalized_anscombe_inverse_mean.md) | Not covered in core suite | None | High-precision diagnostic only (optional mpmath); no strict tail/integration/root certificate. |
| [RES-08A](op_specs/RES-08A_wiener_nsr.md) | ContractFixture, ExactRational | `wiener_delta`, `wiener_singular_minimum_norm`, `wiener_singular_error` | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [RES-08B](op_specs/RES-08B_tikhonov_periodic.md) | ExactRational | `tikhonov_gradient_constant` | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [RES-09A](op_specs/RES-09A_richardson_lucy_poisson.md) | ContractFixture, ExactRational, ExactRationalStaged, ExactSpecial | `rl_nan_zero_prediction_float32`, `rl_nan_zero_prediction_float64`, `rl_delta_one_step`; additional cases in tests.py | Finite staged solver/adjoint; exceptional ratio scalar only, not full exceptional trajectory. |
| [RES-09B](op_specs/RES-09B_richardson_lucy_stabilized.md) | ExactRational, ExactRationalStaged | `rl_epsilon_one_step`, `rl_true_adjoint_dot` | Finite staged solver/adjoint; exceptional ratio scalar only, not full exceptional trajectory. |
| [RES-09C](op_specs/RES-09C_richardson_lucy_poisson_early_stop.md) | Not covered in core suite | None | Predicate and asynchronous runtime not implemented in the oracle; fixed-step recurrence fixtures do not validate this profile. |
| [RES-09D](op_specs/RES-09D_richardson_lucy_stabilized_early_stop.md) | Not covered in core suite | None | Predicate and asynchronous runtime not implemented in the oracle; fixed-step recurrence fixtures do not validate this profile. |
| [RES-10A](op_specs/RES-10A_blind_psf_projected_gradient.md) | ExactRational, ExactRationalInternalState, ExactRationalStaged, ExactSpecial | `blind_psf_direct_float32`, `simplex_nonfinite_float32`, `simplex_nonfinite_float64`; additional cases in tests.py | Finite staged solver and rational simplex; exceptional projection scalar only, not full exceptional trajectory. |
| [RES-10B](op_specs/RES-10B_blind_psf_projected_gradient_early_stop.md) | Not covered in core suite | None | Predicate and asynchronous runtime not implemented in the oracle; fixed-step recurrence fixtures do not validate this profile. |
| [RES-11A](op_specs/RES-11A_low_contrast_smoothing.md) | BitwiseCopy, ExactRational | `deband_flat_range`, `deband_mask_zero_copy` | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [RES-11B](op_specs/RES-11B_pairwise_block_boundary_smoothing.md) | BitwiseCopy, ExactRationalStaged | `deblock_known_boundary`, `deblock_mask_zero_bitcopy` | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [RES-12A](op_specs/RES-12A_paired_notch_denoise.md) | StagedReference | `notch_workflow_zero_depth_impulse` | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [RES-12B](op_specs/RES-12B_gaussian_lowpass.md) | StagedReference | `gaussian_descreen_constant` | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [RES-13A](op_specs/RES-13A_nonflat_ball_opening.md) | DirectedMPFRStaged | `nonflat_ball_constant` | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [RES-13B](op_specs/RES-13B_subtract_background.md) | ExactRational | `background_subtract_signed` | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [RES-13C](op_specs/RES-13C_flat_field_correct.md) | BitwiseCopy, ExactRational | `flat_field_known_reference`, `flat_field_mask_zero_skips_calibration` | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [RES-14A](op_specs/RES-14A_dark_channel_airlight.md) | ContractFixture, DiscreteExact | `airlight_select_first_tie`, `airlight_black_fails_only_when_requested`, `dark_channel_black_without_airlight` | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [RES-14B](op_specs/RES-14B_dark_channel_transmission.md) | ExactRational | `transmission_explicit_omega` | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [RES-14C](op_specs/RES-14C_apply_dehaze.md) | ExactRational | `apply_dehaze_analytic` | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
| [RES-14D](op_specs/RES-14D_dark_channel_dehaze.md) | StagedReference | `dehaze_workflow_constant` | Finite small-array fixtures; no full nonfinite, tensor/Region, resource or runtime coverage. |
