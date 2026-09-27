# 资料核对与设计来源

访问日期：2026-09-26。材料限原作者项目/论文与项目官方文档；滚动页面显示版本是本轮核对记录，不宣称与 oracle 安装依赖相同。本文是摘要和设计对照，不随包复制受限论文、第三方代码或资源。

用户给定的 NUM/FMT 才是项目数值与语义基线；外部资料不覆盖它们。旧 05-filter 只用于提取44个需求ID，不以其实现章节制定兼容目标。

## 主要设计差异

反射命名、median偶数取值、Gaussian中间dtype、FFT归一化、小波phase、TV solver、RL伴随和alpha处理均显式规定；商业功能名称不构成数学定义。论文未规定的precision/shape/error/default由本稿提出，参数、数值和接口以当前英文成员规格及共享契约为准。

## 来源逐项记录

<a id="s01"></a>
### S01 · OpenCV Image Filtering

[原始资料](https://docs.opencv.org/4.13.0/d4/d86/group__imgproc__filter.html)。版本/定位：4.13.0。

filter2D 是相关而非卷积；本稿 FIL-01A/B 明确分开，并将导数 unit-ramp 标度写入公式。

<a id="s02"></a>
### S02 · SciPy ndimage.convolve

[原始资料](https://docs.scipy.org/doc/scipy/reference/generated/scipy.ndimage.convolve.html)。版本/定位：页面显示 1.18.0。

本稿把 half/whole reflection 写成整数映射，避免跨库 reflect/mirror 名称歧义；anchor 和 full/valid global origin 单独规范。

<a id="s03"></a>
### S03 · SciPy gaussian_filter

[原始资料](https://docs.scipy.org/doc/scipy/reference/generated/scipy.ndimage.gaussian_filter.html)。版本/定位：页面显示 1.18.0。

库的分轴中间 dtype 会改变结果。本稿 Gaussian 用明确 RN64 一维权重与 exact 二维外积；radius 由调用者显式提供，不能直接拿库结果当 strict。

<a id="s04"></a>
### S04 · SciPy median_filter

[原始资料](https://docs.scipy.org/doc/scipy/reference/generated/scipy.ndimage.median_filter.html)。版本/定位：页面显示 1.18.0。

库的偶数 median 选上中位，而本稿显式开放 mean/lower/upper 等分版参数；非有限与 tie 另定。

<a id="s05"></a>
### S05 · He, Sun, Tang: Guided Image Filtering

[原始资料](https://people.csail.mit.edu/kaiming/publications/eccv10guidedfilter.pdf)。版本/定位：ECCV 2010，§3.1，公式 5–8，PDF 第4–5页。

公式5–8决定局部回归后还要均值系数，最坏支持2r。本稿 scalar/vector、裁剪窗、epsilon单位和RN64系数是进一步设计。

<a id="s06"></a>
### S06 · scikit-image feature

[原始资料](https://scikit-image.org/docs/stable/api/skimage.feature.html)。版本/定位：页面显示 0.26.0。

沿用 Canny 分阶段/连通问题；本稿量化四向 NMS、平台 tie、阈值包含端点和图外抑制是具名自选版本，不承诺库 bit兼容。

<a id="s07"></a>
### S07 · Paris, Hasinoff, Kautz: Local Laplacian Filters

[原始资料](https://people.csail.mit.edu/sparis/publi/2011/siggraph/)。版本/定位：SIGGRAPH 2011。

采用围绕每目标系数 reference 值做局部 remap，再抽取金字塔系数的参考算法。不能以简单频带 gain 假称 Local Laplacian。

<a id="s08"></a>
### S08 · Jimenez et al.: SMAA

[原始资料](https://www.iryoku.com/smaa/)。版本/定位：Eurographics 2012；作者页列 v2.8。

SMAA 的空间与时间变体不同。仅拟议静态1x，原生算法的 Area/Search 纹理与采样状态必须共同固定；本轮未引入其代码/资源。

<a id="s09"></a>
### S09 · FFTW: DFT definition

[原始资料](https://www.fftw.org/fftw3_doc/The-1d-Discrete-Fourier-Transform-_0028DFT_0029.html)。版本/定位：页面显示 3.3.11。

FFTW 自身正逆不归一。本稿 backward 逆除HW、ortho另定，且 strict transform 的twiddle是数学值而非先bake64。

<a id="s10"></a>
### S10 · FFTW: real-data array format

[原始资料](https://www.fftw.org/fftw3_doc/Real_002ddata-DFT-Array-Format.html)。版本/定位：页面显示 3.3.11。

实DFT只压缩一个轴，原长度奇偶不能从half宽唯一恢复。本稿保存original_shape并对boundary列逐共轭轨道验证。

<a id="s11"></a>
### S11 · SciPy fftconvolve

[原始资料](https://docs.scipy.org/doc/scipy/reference/generated/scipy.signal.fftconvolve.html)。版本/定位：页面显示 1.18.0。

参考线性FFT卷积范围概念；本稿 same/full/valid 和anchor最终对齐直接卷积 exact表达式，不继承库dtype转换。

<a id="s12"></a>
### S12 · SciPy oaconvolve

[原始资料](https://docs.scipy.org/doc/scipy/reference/generated/scipy.signal.oaconvolve.html)。版本/定位：页面显示 1.18.0。

OLA/OLS作为同一卷积的执行profile，block参数不改变答案。数学golden用直接卷积，另测block几何，实际FFT执行未验收。

<a id="s13"></a>
### S13 · SciPy dct

[原始资料](https://docs.scipy.org/doc/scipy/reference/generated/scipy.fft.dct.html)。版本/定位：页面显示 1.18.0。

I–IV的原始矩阵、逆配对和ortho端点权重分别写出；不能把ortho简单理解为原始结果除统一常数。

<a id="s14"></a>
### S14 · SciPy Hann window

[原始资料](https://docs.scipy.org/doc/scipy/reference/generated/scipy.signal.windows.hann.html)。版本/定位：页面显示 1.18.0。

symmetric/periodic显式分开，小尺寸端点单独规定；coherent gain与energy归一不得隐藏。

<a id="s15"></a>
### S15 · SciPy Tukey window

[原始资料](https://docs.scipy.org/doc/scipy/reference/generated/scipy.signal.windows.tukey.html)。版本/定位：页面显示 1.18.0。

Tukey alpha端点与symmetric/periodic作为静态profile；输出窗口的分轴和二维RN阶段明示。

<a id="s16"></a>
### S16 · PyWavelets signal extension modes

[原始资料](https://pywavelets.readthedocs.io/en/latest/ref/signal-extension-modes.html)。版本/定位：滚动文档，版本号未固定。

扩展方式、periodization和symmetric不相同。本稿使用直接lifting公式与保存相位/形状，避免仅传库模式名。

<a id="s17"></a>
### S17 · PyWavelets 2D DWT/IDWT

[原始资料](https://pywavelets.readthedocs.io/en/latest/ref/2d-dwt-and-idwt.html)。版本/定位：滚动文档，版本号未固定。

采用LL/LH/HL/HH的结构思想，但本稿明确LH_y、HL_x角色及三种自定义profile，绝不声称任意PyWavelets basis兼容。

<a id="s18"></a>
### S18 · Buades, Coll, Morel: Non-Local Means Denoising

[原始资料](https://www.ipol.im/pub/art/2011/bcm_nlm/)。版本/定位：IPOL 2011。

patch相似性与search支持是NLM基础；self=1、是否减2sigma²以及反射相关性不做无偏补偿，是本稿明确的不同配置。

<a id="s19"></a>
### S19 · Getreuer: ROF TV using Split Bregman

[原始资料](https://www.ipol.im/pub/art/2012/g-tvd/)。版本/定位：IPOL 2012。

该文用于ROF目标与正则术语；本文标题的Split Bregman不是本稿使用的求解器。本稿独立规定CP每步公式、no-flux真伴随与固定步数。

<a id="s20"></a>
### S20 · BM3D author project

[原始资料](https://webpages.tuni.fi/foi/GCF-BM3D/)。版本/定位：作者页：Python v4.0.3，2024-09-06。

算法与软件许可分离。作者实现有用途限制，本轮不复制软件、不生成冒充真实引擎的golden；采用原生实现并与固定第三方实现对照，差异需逐项解释。

<a id="s21"></a>
### S21 · Mäkitalo, Foi: inverse Anscombe research

[原始资料](https://webpages.tuni.fi/foi/invansc/)。版本/定位：2011/2013 论文与作者项目。

均值域逆不是简单平方代数逆，也不是任意denoiser的普遍无偏保证。严格inverse需要尾/积分/求根证书，本轮两函数明确只有诊断级。

<a id="s22"></a>
### S22 · scikit-image restoration

[原始资料](https://scikit-image.org/docs/stable/api/skimage.restoration.html)。版本/定位：页面显示 0.26.0。

用于比较恢复术语与模型。Wiener NSR、Tikhonov、RL epsilon与true adjoint、finite-step TV分别定版；不继承隐藏clip/归一默认。

<a id="s23"></a>
### S23 · He, Sun, Tang: Single Image Haze Removal

[原始资料](https://people.csail.mit.edu/kaiming/cvpr09/index.html)。版本/定位：CVPR 2009 / TPAMI 2011。

DCP是先验估计，不是实测透射率/depth。本稿linear RGB域、top-k tie、airlight选整像素和apply分离是明确接口选择。

<a id="s24"></a>
### S24 · scikit-image rolling-ball example

[原始资料](https://scikit-image.org/docs/stable/auto_examples/segmentation/plot_rolling_ball.html)。版本/定位：页面显示 0.26.0。

空间半径与强度height不同量纲。本稿定义nonflat min/max opening版本，并未声称所有rolling-ball软件行为相同。

<a id="s25"></a>
### S25 · GNU MPFR manual

[原始资料](https://www.mpfr.org/mpfr-current/mpfr.html)。版本/定位：页面显示 4.2.2。

用向下/向上舍入建立区间，再直接判定目标IEEE bits；MPFR宽指数域不能代替目标subnormal规则，最终转换必须按binary32/64。

<a id="s26"></a>
### S26 · scikit-image filters

[原始资料](https://scikit-image.org/docs/stable/api/skimage.filters.html)。版本/定位：页面显示 0.26.0。

用于LoG/DoG、Gabor的数学概念；本稿零DC修正、复响应方向和尺度规则独立明确，不复用库默认和像素容差。

## 软件许可与对照资源

BM3D/CBM3D及inverse-Anscombe作者软件的使用条件需按目标用途审阅；数学论文公开不等于作者binary可以商业分发。本包只含本轮独立写的参考程序与引用，没有这些作者软件，也没有SMAA shader/纹理。MPFR、mpmath及诊断性NumPy/SciPy由运行环境独立安装，使用者应管理其各自版本与许可证。

## 尚未闭合的研究/工程问题

两个mean-inverse的严格尾界与root-rounding证书未完成；SMAA/BM3D/CBM3D 的具体第三方对照版本和资源尚未固定、未运行；没有测量strict transform和exact迭代在生产大小下的性能。上述限制写入每成员和coverage，而不是以近似数学或旧实现掩盖。

<a id="s27"></a>
### S27 · Chambolle and Pock: A First-Order Primal-Dual Algorithm

[作者预印本](https://www.cmap.polytechnique.fr/preprint/repository/685.pdf)。
ROF 的原始—对偶形式用于核对 TV 目标、真正伴随及可行对偶下界；本项目的
阶段舍入、按像素归一间隙、异步检查和终止协议由英文规格独立定义。
