# 频域、去噪与恢复

状态Proposed。DFT/固定卷积/具名基础恢复为D1数学核心，复杂统计和迭代质量选择为D2。signed real与complex语义依赖G1/G2，FFT和全局求解优先Whole，不能把任意频率滤镜误标有限Halo。

## 频域基础

| ID / 功能 | 输入 → 输出 | 建议参数与数学 | 支持与验收 |
| --- | --- | --- | --- |
| FRQ-01 fft2/ifft2 | real/complex→complex | axes=y,x；forward负号不缩放，inverse除HW；ortho另mode | O(PC logP)，W；round-trip/Parseval/impulse |
| FRQ-02 rfft2/irfft2 | real↔half complex | `[H,floor(W/2)+1,C,2]`实虚表示建议，保存原W | 奇偶宽、Hermitian、DC/Nyquist |
| FRQ-03 fftshift/ifftshift | frequency array→重排 | 仅移频率索引，不改变谱值 | 奇数长度shift与inverse不混用 |
| FRQ-04 window | field+shape→windowed field/window | Hann/Tukey等具名、periodic/symmetric、axes | 窗能量、coherent gain；谱分析另行归一 |
| FRQ-05 frequency response | shape+sampling→complex/real Hf | Gaussian/Butterworth/ideal low/high/band/notch；截止cycles/unit | DC、截止、Hermitian；硬截止产生长响应/ringing |
| FRQ-06 frequency multiply | F+Hf→F | 复乘，通道广播显式 | 实结果需共轭对称；elementwise辅助shape适配 |
| FRQ-07 fft convolve | real+fixedK→real | linear/circular、anchor、same/full/valid | 完整linear零填充尺寸至少输入+核-1；circular固定目标周期并按周期放置/折叠kernel；各自对直接oracle |
| FRQ-08 block convolution | real+K→real | overlap-add/save，块大小及halo | 固定平移不变核，整图和分块一致 |
| FRQ-09 dct/idct | field↔coefficients | type I/II/III/IV、norm和轴显式 | 配对重建；block边界 |
| FRQ-10 wavelet decompose/reconstruct | field↔bands | wavelet、levels、边界、decimated/undecimated | 无修改重建、相位/边界；多shape G3 |

FFTW使用不归一的正逆DFT，其他库可采用unitary；适配层必须实现公开norm而非直接继承库默认。实DFT压缩格式需要保存原始长度。[^fftw] 线性FFT卷积与overlap-add均可对齐直接oracle；大核未必应选择FFT，box和separable另有低成本路径，阈值依真实尺寸、ROI和设备测量。[^scipy]

## 去噪与修复目录

| ID / 功能 | 输入 → 输出 | 算法与参数 | 验收/依赖 |
| --- | --- | --- | --- |
| RES-01 noise estimate | field+mask?→sigma/variance map | MAD/high-frequency/calibration具名，假设噪声域 | 纹理污染、常量、zero valid samples；W/H |
| RES-02 NLM | field+noise model→field | patch/search radii、h、distance权重；CPU参考 | O(P·patch²·search²)直接；fast variant须注明语义变化 |
| RES-03 wavelet denoise | field+sigma→field | basis/levels、soft/hard threshold rule，固定cycle shifts | detail bias、纹理与平移不变等级 |
| RES-04 BM3D/CBM3D | gray/color+sigma→field | block matching、group size、两阶段协同滤波 | CPU先行；一般体数据BM4D与视频V-BM3D/V-BM4D分别定义 |
| RES-05 TV denoise | field→field | `0.5*sum((u-f)²)+lambda TV(u)`、solver/iterations/tolerance | 目标函数/残差、纹理损失；W或协调迭代 |
| RES-06 anisotropic diffusion | field→field | conductance函数、dt、steps、边界 | 稳定步长、边缘保留；不与TV参数互用 |
| RES-07 variance stabilize/inverse | count/linear calibrated→稳定场↔恢复 | Anscombe/GAT；gain、offset、read noise | 低计数bias，inverse不默认简单平方 |
| RES-08 Wiener/Tikhonov | observation+PSF+regularizer→estimate | lambda、regularizer频响、边界、norm | signed/HDR；重投影与已知噪声，不clip |
| RES-09 Richardson–Lucy | nonnegative observation+PSF→estimate | iteration、background、epsilon、adjoint、stopping | 非负，噪声放大；masked边界分母处理 |
| RES-10 blind PSF estimate | observation+prior→PSF/estimate | 支撑、归一、非负、正则、多尺度 | 不适定，D2；不能当普通锐化 |
| RES-11 deband/deblock | image+mask?→image | 量化带与block边界模型分开 | 细纹保护、误检mask、mask外保持 |
| RES-12 moire/descreen | image→image | notch频率/角度、局部方向或模型 | 真实重复纹理损失、剩余频峰；网点还原独立 |
| RES-13 background/flat field | image+reference?→background/corrected | rolling-ball/大尺度估计与实测flat分开 | 除零/单位、真实低频结构保护 |
| RES-14 dehaze estimate/apply | RGB+airlight/transmission?→RGB+fields | `I=Jt+A(1-t)`；估计与应用分开，t_floor显式 | 天空/亮区域先验失效，t不声称实测depth |

局部inpaint、Poisson与PatchMatch在[修补规格](../09-composite/keying-paint.md)，避免重复建不一致接口。摄影亮度/色度降噪可组合color model→分别denoise→merge；算法噪声标度仍需匹配所选模型。

## 恢复数学

Wiener–Hunt/Tikhonov候选：`x=IFFT(conj(H)*FFT(y)/(|H|²+lambda|D|²))`。D是正则算子，不等于直接填写noise-to-signal ratio；二者用不同参数名。

一般非负成像算子A的数值稳定RL变体：`x_next=x * Aᵀ(y/(Ax+b+epsilon))/(Aᵀ1)`。要求非负初始化，epsilon>0是显式稳定化参数；它改变严格无epsilon迭代。mask、边界和空间变PSF都属于A；零sensitivity `Aᵀ1=0`位置默认保持初值并标无观测，或显式交由先验处理。只有满足归一条件才省略分母。必须以点积试验确认forward/adjoint匹配，迭代次数也构成正则化选择。每次forward+adjoint扩大依赖，不能把一次卷积halo复用为全部迭代需求。[^restoration]

NLM使用patch相似性，有限搜索的支持至少search radius+patch radius；块估计再聚合要额外扩张。[^nlm] BM3D静态灰度、彩色与视频变体输入契约不同。[^bm3d] TV按明确目标lambda定义，库里的weight可能取反比；验证前不可直接复用同数值。[^tv]

Poisson/Gaussian相机噪声应记录electron/gain/read-noise，方差稳定→Gaussian denoise→有bias控制的inverse是可复用链。[^anscombe] 去雾Dark Channel Prior为统计先验，对天空等区域需失效例，估计transmission不视为真值。[^haze]

## 验收与使用

最小流程：`known image→fixed PSF→seeded noise→Wiener/RL→forward reproject→residual/scopes`；`noise estimate→denoise→residual+PSNR/SSIM`；`pyramid→bands unchanged→reconstruct`。FRQ的可逆数值结果与用于观看的log-magnitude图必须分开。

FFT验收检查impulse全频、单频正弦、DC、Hermitian、odd/even、linear vs circular卷积和归一；恢复检查低光bias、边缘/纹理、mask外保持、正则强度变化和独立噪声。CPU/Metal使用相同参数与明确容差；benchmark给尺寸、核/迭代、临时空间，不能仅报理论复杂度为实测速度。

## 来源

[^fftw]: FFTW，[*DFT Definition*](https://www.fftw.org/fftw3_doc/The-1d-Discrete-Fourier-Transform-_0028DFT_0029.html)、[*Real-data Array Format*](https://www.fftw.org/fftw3_doc/Real_002ddata-DFT-Array-Format.html)，访问3.3.11。
[^scipy]: SciPy，[*fftconvolve*](https://docs.scipy.org/doc/scipy/reference/generated/scipy.signal.fftconvolve.html)、[*oaconvolve*](https://docs.scipy.org/doc/scipy/reference/generated/scipy.signal.oaconvolve.html)，滚动官方API。
[^restoration]: scikit-image，[*Restoration API*](https://scikit-image.org/docs/stable/api/skimage.restoration.html)，滚动文档；迭代/正则/clip行为，Photospider默认另定义。
[^nlm]: Buades、Coll、Morel，[*Non-Local Means Denoising*](https://www.ipol.im/pub/art/2011/bcm_nlm/)，IPOL，2011-09-13。
[^bm3d]: Dabov等，[*BM3D author project*](https://webpages.tuni.fi/foi/GCF-BM3D/index.html)，原论文2007及后续变体。
[^tv]: Getreuer，[*ROF Total Variation Denoising using Split Bregman*](https://www.ipol.im/pub/art/2012/g-tvd/)，IPOL2012。
[^anscombe]: Mäkitalo、Foi，[*Anscombe/GAT inversion research*](https://webpages.tuni.fi/foi/invansc/index.html)，2011/2013。
[^haze]: He、Sun、Tang，[*Single Image Haze Removal using Dark Channel Prior*](https://people.csail.mit.edu/kaiming/cvpr09/index.html)，2009/2011。
