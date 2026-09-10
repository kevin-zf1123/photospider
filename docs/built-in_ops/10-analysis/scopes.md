# 数值分析与示波器

2026-09-11：本轮基础子集的接口、Region 与可运行示例见[基础算子实现](../../kernel-architecture/zh/Basic-Operations.zh.md)。其他目录项继续保持原研究状态。

状态Proposed。统计、直方图、频谱和明确坐标的scope为D1数学核心，复杂相似性、光学测量与交互图为D2。输出先是数值结果，`scope.render`再产生可显示图像。scope亮度/颜色/网格不能改变底层统计。

## 公共分析配置

每次分析显式记录signal domain（RAW/scene linear/working/display）、primaries/white/transfer、viewer transform是否应用、ROI/data window、mask权重和alpha策略、数值范围和单位。默认建议分析原始绑定值、完整指定ROI、exact而非预览抽样；颜色feature必须显式选择。

finite/NaN/Inf/无效样本数、underflow/overflow、总count/weight均返回。忽略透明、alpha-weighted、composite-over指定背景是不同模式；只对premul除alpha后把透明噪声纳入统计不是合理隐式默认。全无效样本返回明确valid=false/错误策略，不发布貌似均值0的结果。

Nuke scopes公开提供REC601/709编码、viewer transforms和full-frame开关，说明这些选项会影响读数。[^nuke] AE Lumetri也有独立颜色状态，显示器亮度外观不能替代数值域定义。[^ae]

## 目录

| ID / 数值操作 | 输入 → 输出与参数 | 算法/依赖 | 验收 |
| --- | --- | --- | --- |
| ANA-01 statistics | field+mask→每channel min/max/sum/mean/variance/std/count | Float64稳定累加，ddof=0建议，weighted规则显式；W | 已知数组、极大偏置小方差、无效计数 |
| ANA-02 quantiles | field→quantiles | q列表、插值规则、exact/approx具名；W | median/排序值，近似误差 |
| ANA-03 histogram | field→counts[C,B]+edges/溢出 | B=256建议，domain必填，最后bin含上界；W | 总计数守恒、端点、HDR溢出 |
| ANA-04 histogram2d | feature1/2→counts[B1,B2] | 两轴range、bins、mask | joint分布和后续vectorscope共用 |
| ANA-05 waveform | signal→counts[Xbin,Vbin,signal] | 保留x位置，沿y聚合；preview横轴bin具名 | 窄竖条、横向位置与值的解析落点 |
| ANA-06 parade | waveform data→排列视图/统计描述 | RGB/YRGB/YCbCr信号分别配置 | 通道排列，不另造不同统计算法 |
| ANA-07 vectorscope | declared RGB→CbCr density+axes | matrix/range/transfer、75/100%target overlays | neutral中心、色条矩阵oracle |
| ANA-08 chromaticity scope | XYZ→xy或u′v′ density | observer/white、gamut overlay | 零分母无效、primary坐标；不等于视频vectorscope |
| ANA-09 clipping/gamut report | image+target→mask/count/extrema | numeric range/gamut/display-nits三类独立 | 超范围像素定位、既不自动clip也不改变颜色 |
| ANA-10 false color/exposure map | scalar feature→color image | thresholds、stops基准或nits、色表必填 | 每个阈值边界与legend，viewer前后区别 |
| ANA-11 difference | aligned A,B→signed/abs/squared map+stats | domain、alpha、ROI对齐；E+W | identity0、局部差异位置，signed不丢 |
| ANA-12 MSE/RMSE/PSNR | A,B→metrics | data_range显式，channel/mask聚合 | MSE=0时PSNR约定+Inf/valid标记；HDR不取dtype最大值 |
| ANA-13 SSIM | A,B→map/metric | window、sigma、K1/K2、data_range、boundary | 特定算法版本、通道策略、解析常量与参考集 |
| ANA-14 DeltaE | common-white Lab→map/stats | ΔE76/94/2000具名，观察条件一致 | ΔE00公开补充测试集、灰轴/角度难例 |
| ANA-15 spectrum | complex FFT→magnitude/phase/power | shift、log floor、DC处理 | 零幅相位无定义；显示图不是可逆频域数据 |
| ANA-16 PSD | field+spacing/window→density+frequency axes | 去均值/趋势、window、norm、one/two-sided、平均法 | 能量积分、正弦、补零、采样单位 |
| ANA-17 radial spectrum | 2D spectrum→ring values/counts | ring mean或integral，frequency metric | 各环样本数量；不同单位不能混用 |
| ANA-18 focus/edge profile | image/ROI→focus score/map/profile | Laplacian/gradient/MTF方法具名 | 噪声与锐化偏差；不是通用画质评分 |
| ANA-19 mask/label measurements | mask/labels→area/bbox/centroid/moments/perimeter | connectivity、pixel spacing、binary/coverage area | soft面积为sum coverage；perimeter定义显式 |
| ANA-20 PSF/MTF analysis | PSF→centroid/energy/OTF/MTF | normalize、radial/axis、cutoff/encircled energy | unit energy、对称PSF、lambda尺度 |
| ANA-21 scope render | 数值结果+style→RGBA图 | axis/legend/grid、linear/log density、gain仅显示 | 同数据不同显示gain不改变counts，刻度与单位可读 |

多项统计是结构化结果需求，G3须决定单Value承载或分节点。CPU参考归约有固定顺序/稳定算法；整数计数使用checked Int64或明确更大容量，weighted counts建议Float64。GPU局部histogram再合并可减少atomic竞争，但exact和近似预览是不同质量模式。当前内核不自动提供任意跨tile reduction，首版Whole须给内存上界。

## 定义与归一化

Histogram count是频数；probability按in-range总权重归一满足sum=1，density再除bin_width满足`sum(value*bin_width)=1`。in-range权重为0时返回valid=false，保留零counts与under/overflow计数，不执行归一除法。内部区间左闭右开，最后一个bin含最右端点；超范围单独计数而非悄悄裁切。

线性Y来自RGB→XYZ矩阵，编码Y′由指定视频系数定义。BT.70910-bit nominal luma/RGB为64..940、chroma为64..960，中性512，不能所有通道统一写64..940。[^itu] vectorscope坐标应先在数学域明确，再编码/归一；skin guide只是显示参考，不是自动识别人脸或正确肤色的标准。

二维DFT采用正变换不缩放，采样间隔dx,dy，window=w。一个可实现的双边PSD定义为：

`P=dx*dy*|DFT(w*x)|²/sum(|w|²)`，`dfx=1/(Nx*dx)`，`dfy=1/(Ny*dy)`。要求窗能量>0，Nx/Ny为实际FFT长度。

因此`sum(P)*dfx*dfy=sum(|w*x|²)/sum(|w|²)`，也适用于复数输入。若已去均值，这用于相应能量/方差检查；有窗时是窗口加权量，不能无条件称为原图未加权方差。FFT其他norm必须换算。rfft2按压缩轴内部列乘2，DC列及偶数长度Nyquist列整列乘1，另一轴不再乘2。补零时频率间距用FFT长度，窗能量仍取实际数据窗。补零不增加原观测的频率分辨能力。periodogram/Welch参数可参考SciPy。[^psd]

频谱magnitude的coherent-gain与PSD的window-energy归一不同。环带均值PSD与环带积分功率也不同；单位包含cycles/px或cycles/mm，二者依据spacing转换。analysis window不是用于卷积的PSF，两种核不要共享隐式归一。

PSNR定义为`10log10(data_range²/MSE)`。DeltaE必须在一致白点/观察条件中测量，ΔE00使用Sharma等补充数据验证角度与低chroma难例。[^delta] 有损gamut转换不应以往返bit identity验收。

## 使用面与验收

2×2标量`[0,.25;.5,1]`在[0,1]四个等宽bin中输出[1,1,1,1]。常量图无window且不去均值时只有DC，impulse全频常幅，整数周期正弦在指定bin出现峰。全红图RGB parade的R高G/B低，vectorscope按选定矩阵计算位置。

`source→histogram→auto exposure parameters→grade→waveform`；`image→FFT→PSD→radial average`；`PSF→MTF/energy`；`two images→ΔE/difference→mask report`。数值输出供workflow继续消费，渲染图服务检查与导出。GUI交互选择区不属于分析kernel，但ROI与参数须显式传入。

## 来源

[^nuke]: Foundry，[*Using Scopes*](https://learn.foundry.com/nuke/content/timeline_environment/usingviewer/using_scopes.html)，滚动官方指南。
[^ae]: Adobe，[*Color Basics*](https://helpx.adobe.com/mena_en/after-effects/desktop/adjust-colors/color-basics/color-basics.html)，英文AE指南。
[^itu]: ITU-R，[*BT.709-6*](https://www.itu.int/dms_pubrec/itu-r/rec/bt/R-REC-BT.709-6-201506-I!!PDF-E.pdf)，2015-06，Digital representation。
[^psd]: SciPy，[*periodogram*](https://docs.scipy.org/doc/scipy-1.16.0/reference/generated/scipy.signal.periodogram.html)，1.16.0；[*welch*](https://docs.scipy.org/doc/scipy/reference/generated/scipy.signal.welch.html)，访问1.18.0。
[^delta]: Sharma、Wu、Dalal，[*CIEDE2000 supplementary test notes*](https://hajim.rochester.edu/ece/sites/gsharma/ciede2000/ciede2000noteCRNA.pdf)，2005。

接口拆分参考：OpenImageIO [ImageBufAlgo](https://openimageio.readthedocs.io/en/stable/imagebufalgo.html)，稳定文档访问3.1.17；PixelStats、compare与FFT各自独立，其unitary FFT需适配本规范norm。
