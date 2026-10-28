# 频域、去噪与恢复

状态：Proposed；不沿用旧 FFT/Bands 实现、自动分页策略或 duplicate-last 旧 payload ABI。具名数学 profile 自己规定 shape、stage、边界和 owner；原生 BM3D/CBM3D/SMAA 的数学配置和第三方对照仍须落实，不表示已可运行。

本页列出频域与恢复族；[空间滤波、边缘与细节](spatial.md)列出其余 20 族。

FRQ-01～06 的 14 个成员已提供 CPU 实现与显式 FrequencyGrid SDK helper，共 30 个注册键。FRQ-07～10 的 12 个成员仍未实现；规格状态继续为 Proposed。

FFT/IFFT/RFFT/IRFFT 使用普通浮点算法，注册名为 `frequency.fft2`、`frequency.ifft2`、`frequency.rfft2`、`frequency.irfft2`。四个变换不提供 strict/accelerated 选择；SDK `transform(document, member, inputs, grid)` 使用同一契约。Float32 输入使用原生 binary32 工作值、蝶形、累加、实数重组与输出缩放，Float64 使用 binary64。尺寸决定的三角系数与归一化常数先以 binary64 生成，再转换为工作 dtype。普通浮点舍入、NaN payload、零符号和中间溢出遵循实际计算；Float32 的中间动态范围也属于单精度，不会按输入值自动提升精度重算。小残差与不同算法、平台间的差异继续按原有误差阈值验收，不承诺与 OpenCV/MATLAB 逐位相同。

Float32 的 16～128 点二次幂行变换使用固定自然序 codelet；工作行宽可被四整除时，16～65536 点二次幂列变换使用四 lane 蝶形与紧凑缓冲区内的原地 DIF 递归。x86 每个任务处理八列，Apple AArch64 从高度 256 起处理十六列以覆盖 128 字节缓存行；较小高度使用八列，不足完整带宽的末段仍以四列为单位使用同一有界路径。Intel 的 16384～65536 点 Float32 行变换使用平衡的双因子分解。其他二次幂轴保留准备好的 radix-2/radix-4 计划。AArch64 的 64/128 点 Float32 行变换将内部 16/32 点叶子的实部、虚部分别保存在向量包中，外部复数存储仍为交错布局。

Float64 的 8～128 点二次幂轴使用原生两复数向量包。8/16 点行叶子在寄存器内完成，32/64/128 点行变换递归分成四路；列变换使用有界 DIF 递归并直接写回自然序，该路径还覆盖两个平台的 1024/2048 点以及 x86 的 256/512 点列轴。更大的行轴保留 prepared power plan，连续单列在以下单线并行情形之外按行轴策略选择。工作行宽为奇数时，偶数列使用原生包，最后一列使用原有标量 mixed plan。128 点行变换需要 160 个复数的临时空间，列带最多需要高度八倍的复数空间，均由公共 worker 预留覆盖。各层共用按 Context 资源计量的 binary64 根表。

工作平面只有一条 FFT 线时，16384～65536 点二次幂轴在线程池有多个参与者时使用双因子并行路径，两个因子均为 128 或 256。第一阶段将互不重叠的输入列带变换到私有紧凑自然序区，再转置并乘系数，写入 N 个复数的共享中间区；系数按中间区的行序预排，可连续加载向量包。全部 join 后，第二阶段直接向原工作平面写入自然序结果，不再整轴复制。Float32 使用 column-mode 固定计划，Float64 使用匹配因子的 128/256 点小根表。系数矩阵按长度、方向和精度缓存并计入 Context 资源，在该路径替代原整轴根表；只有构建完整的表才可发布，取消的调用仍可能留下完整可复用表。线程池上限为一时保留原串行路径。

共享中间区与所有私有带工作区合计不超过 2N 个复数。x86 的完整带宽为八列，Apple AArch64 为十六列；第一阶段使用一半带宽，将每个 worker 原有工作区等分为原生 scratch 和紧凑输出区，第二阶段使用完整带宽。参与数同时受列带数与实际 scratch 限制，并按两个阶段的声明总工作量统一选择；Apple AArch64 上该总量低于 2^24 时，两个阶段均最多使用四个参与者。N=A×B、原生列费用为 C 时，两阶段费用分别为 A×C(B)+64N 和 B×C(A)，缺失缓存的系数矩阵构建准入 128N 单位；算术与转置的单段继续不超过 1024 单位，不新增 Run 内分配。

非二次幂使用混合基数计划、最大基数为 31 的奇数 codelet，以及 Rader 或 Bluestein 卷积。因子排序尽量让二次幂阶段先执行，并融合初始八点蝶形；radix-5 展开调度在载入向量前检查取消，减少跨回调保存的寄存器。x86 的 radix-7/11/13 也使用显式展开，将取消检查移到完整输出对之前，保留总工作预算；AArch64 对这些奇数基数保留较小的调度。分解识别 257 以内的素因子，较大的真因子作为单独卷积阶段执行。残余长度的 n−1 可由小因子分解且有界搜索证明存在原根时使用 Rader；其余使用 Bluestein。卷积的正向 DIF 和逆向 DIT 共用排列后的核频谱。x86 同时检测 AVX 与 FMA，AArch64 使用 NEON；其他目标保留标量实现。显式 FMA 属于普通浮点契约，编译仍使用严格浮点选项。

全部因子可由有界小基数分解的轴计划在互素阶段边界使用 Good–Thomas 排列，省去该边界的 twiddle 复乘。排列与逆频率映射属于不可变 prepared plan，递归卷积计划保留自身布局。最终普通 radix-3/5 列阶段在包宽不超过四且满足计划条件时，将蝶形与自然序写回融合，减少一次 scratch 写入和独立散写；该路径同时支持 Float32 和 Float64。x86 的 Float64 mixed 列轴从长度 256 起，当完整计划及其叶卷积仅使用 radix-2/3/4/5，且行宽可被四整除或存在叶卷积时，可按四个交错复数 lane 处理；不足四列的尾部保留两 lane/标量路径，行轴独立选择。该属性与 Float32 所用的原 wide-plan 标志分离。

固定列 codelet 之外的列变换按尺寸选择直接 SIMD、合并位反转的 gather，或 H+1 间距的分块转置。转置带最多保存 64 列，x86 从高度 4096 起最多八列；细长平面可进一步缩窄以分配并行任务。偶数宽度 RFFT/IRFFT 使用 W/2 点复数行变换与 H×(W/2+1) 紧凑平面，奇数宽度使用完整复数平面。scratch 按尺寸和精度预留独立的参与线程工作区，并覆盖标量回退所需空间。高度不足 256、单列、所选原生 Float64 列和高度小于 8192 的非二次幂列只需行工作区；其他转置区按实际最大列带与工作行宽的较小值预留。符合平台 power 列门槛且行宽可被四整除的二次幂轴不需要独立转置区；Apple AArch64 对尚未由原生 Float64 路径覆盖的 256～65536 点对齐二次幂列，额外保留至少 8H 个复数，与行工作区合计覆盖原生 Float32 的十六列带，包括细长矩阵。工作平面面积 HP 小于 4096 时只预留一份，4096≤HP<16384 时最多八份，其他尺寸最多 32 份，同时受独立行列数量限制；即使运行时指定一个线程也保留该尺寸上界。运行时线程设置不改变 Plan 的容量要求。复数工作值分别按 Float32 的 8 字节和 Float64 的 16 字节计量。prepared kernel 缓存两种精度的不可变排列、根表、分级系数以及卷积核，并计入 Context 资源。切换实现或编译目标时须重新生成 Plan。完整且 x 连续的输入提前解析各自的仿射行地址，其他布局按 tile 缓存行基址。两个输出视图匹配的复数变换在一次扫描中写出实部和虚部，分别保留各自地址与行步长；其他输出选择逐视图写回。成对写出为每个复数样本声明 32 个标量工作单位、每行 16 个单位，准入、本地份额与取消检查使用相同上界。连续单列工作平面在各输入行步长均为一个标量时按连续段打包；匹配的输出使用连续分量转换，成对复数输出每块最多 32 个样本检查行开销，总声明费用不变。RFFT 边界构造和紧凑 IRFFT 解包继续使用各自路径。x86 上，完整连续单行或标量行步长的单列输入达到 32768 个待打包样本时，可按 2048 个样本分块并行，参与数不超过 min(线程池容量, 8, floor(样本数/8192))；任务块由参与者动态领取，该人数规则不保证每个 worker 实际处理固定数量的样本。各块具有独立且精确的 Work 份额。紧凑实数输入按实数对计数，所有块 join 后才构造末尾零；紧凑逆变换包含输入的 Nyquist 元素。其他布局及 Apple 输入遍历保留原路径。负步长、非对齐地址、选轴与批次支持保持不变。RFFT 构造边界共轭对，自共轭虚部为 +0；IRFFT 按调用方的边界共轭前提重构，支持独立编辑的内部系数。实数路径均按原始 H×W 归一化。

FRQ 线程池默认按硬件并发数选择参与线程，最多 32 个，包含调用线程；Linux 还按首次构造时调用线程可用的 CPU 数限制默认值。`PHOTOSPIDER_FRQ_THREADS=N` 在首次构造时读取，显式正整数上限也截断到 32。上述单线双因子路径之外，一般轴选择器在阶段声明工作量小于 2^20 时保持串行，达到门槛后按每 2^19 单位一个参与线程选择数量，并受池容量、独立任务数和 scratch 份数限制；Apple AArch64 上这些一般轴阶段的声明工作量低于 2^24 时最多使用四个参与线程。装载、实数重组与输出保留独立门槛。小规模串行阶段不构造线程池，独立任务数还会进一步限制实际参与数。x86 的装载、实数重组与输出阶段在声明工作量达到 2^28 个标量单位时额外限制为最多八个参与线程，以减少内存争用；FFT 阶段独立选择参与数。宿主取消查询借用 Run 已持有的 token，每次轮询不再复制共享 owner。该设置独立于宿主 Run 的 lane 上限，不绑定 CPU。

x86 为每个 FRQ worker 使用独立条件变量，每个阶段只通知实际参与线程，销毁时通知全部线程。AArch64 使用共享条件变量：全池参与时广播并保留物理线程与 scratch 槽的对应关系，部分参与时在发布任务的锁内只通知所需数量的等待者，由它们为本次任务领取唯一逻辑槽。任务完成、异常排空和销毁仍遵守原有 join 边界。

线程池忙时，并发 invocation 使用自己的调用线程继续执行，因此线程池上限不是进程全部并发调用的总线程上限。各阶段由调用线程一次性准入工作额度，参与线程使用独立的有限额度与 scratch，保留原取消查询；返回前全部排空，并在正常及异常路径检查宿主的首个失败状态。线程恢复各自的浮点环境，池中不保留 Run 数据。实际扩展率取决于阶段粒度、P/E 核与 SMT、缓存和内存带宽，应通过指定线程配置测量。

mixed plan 的 double radix-4 对标量和不同包宽统一采用每个独立复数 lane 160 单位的上界，融合八点阶段的折扣由当前 radix-2/radix-4 费用计算。相邻 double 轴阶段合并重复的完成查询，每个实际计算段仍先检查，轴结束仍保留完成查询。

短 double 行内核的费用为 N≤16 时 40N，其余为 72N+4R(N/4)；列内核为 N≤8 时 32N，其余为 48N+4C(N/4)。行 gather/combine 段分别声明 256/320 单位，两列 radix-4 段声明 384，最大不可分叶子段为 640，计入构造、标量控制及 AVX/NEON 布局操作。原生偶数列与可选标量尾列按各自成本分配精确且互不重叠的份额。Float64 原生列仅按偶数列的工作量选择参与数，该部分不超过 2^20 单位时保持串行，标量尾列不会单独触发临界规模的并行阶段。

typed CPU invocation ABI9 继续对同一 Run 累计工作量。确定成本的块在执行前准入，块内及块间每不超过 1024 个标量单位检查取消；1024 不是单次准入请求的上限。节点 `max_work` 分别限制每次 invocation。四个变换不消耗精化阶段，冷启动也可使用 `psx --max-stages 0`；其他 FRQ 成员保留各自的严格或加速数值契约。参数、内存与执行边界见 [FRQ-01](op_specs/FRQ-01_contract.md#runtime-integration-and-remaining-acceptance)。

执行器构造 Result 时使用 `max(1 GiB, ExecutionContextConfig::maximum_live_bytes)` 聚合增长上限，计入保留的 backing 容量及发布元数据。默认值仍为 1 GiB；提高 Context 的 Payload 配置也会提高执行器 Result 的增长上限，Host、Payload 和 Metadata 各自的资源限制继续生效。配置能够同时容纳两份输出、scratch 和元数据时，可发布完整 16384×16384 Float32 输出。显式选择较小输出区域仍保留完整变换依赖。

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

DFT backward 正向不缩放、逆向除 HW，ortho 独立；半谱只压缩 x 轴并保存原始奇偶宽。Hermitian 共轭关系由调用方保证；显式投影是独立算子，逆变换不扫描校验或静默丢弃虚部来修复输入。FFT/OLA/OLS 卷积的 strict 数学与直接卷积一致，执行 strategy 不产生新的舍入自由度。DCT I–IV 的矩阵和 inverse/ortho 权重明确；首版小波是三个具名 profile，不接受任意 basis 字符串。

恢复中 Wiener NSR 与 Tikhonov 正则不同；RL 必须使用有限 A 的真实 Aᵀ 和 sensitivity，并明确每步 RN。每种迭代算法同时提供固定 `max_iterations` profile 与独立早停 profile；固定 profile 不暗中早停。Anscombe 代数逆和均值域逆分版；后者两项 oracle 仅诊断级。BM3D/CBM3D 定义为自行原生实现，并须与固定版本的第三方实现对照；当前尚未指定对照实现或版本，不据此宣称已完成对照。

图像语义、直通 copy、有限域和精度详见 [共有契约](op_specs/FILTER_common_contract.md)；结构输出见 [集合契约](op_specs/FILTER_collections_contract.md)。[独立 oracle](../../../oracle/ops/filter/README.md) 不代表 runtime/ISA 验收。

独立参考的逐成员范围见 [oracle 覆盖](oracle-coverage.md)。
