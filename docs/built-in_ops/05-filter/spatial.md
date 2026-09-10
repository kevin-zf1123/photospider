# 空间滤波、边缘与细节

2026-09-11：本轮基础子集的接口、Region 与可运行示例见[基础算子实现](../../kernel-architecture/zh/Basic-Operations.zh.md)。其他目录项继续保持原研究状态。

状态Proposed。确定核/局部统计/导数为D1，复杂保边、多尺度和后处理AA为D2。通用核输入为signed real `[H,W,C]`和kernel `[Kh,Kw]`；颜色wrapper另负责alpha/transfer。CPU参考Float64累加，输出Float32/64；GPU支持须逐算法验证。

## 共同参数

kernel anchor、convolution/correlation方向、normalization(none/sum/l1)、bias、same/full/valid与输出原点均显式。奇数核可默认中心，偶数核要求anchor。零和导数核不得sum归一化。边界模式要定义短数组延拓，不能只写reflect。

非负归一模糊同时处理linear premul RGB和alpha；一般signed卷积、导数、高通不自动卷积alpha并反预乘。mask有两个不同用途：`mix(I,filter(I),M)`限制应用结果；对非负可归一权重核，`sum(KMI)/sum(KM)`限制参与样本并需要零权重策略。signed核的缺失样本处理另定，不能套此分母；导数零和不表示没有样本。

OpenCV `filter2D`实际为correlation，调用库时必须翻核/anchor才能实现convolution，这是第一个方向性验收。[^opencv]

## 算子目录

P=H×W，A=Kh×Kw；含C的公式已计入通道数，其他公式按单通道表示。

| ID / 功能 | 参数、算法和复杂度 | 依赖 / 支持 / 验收 |
| --- | --- | --- |
| FIL-01 convolve2d/correlate2d | input+K→field，默认normalization=none；直接O(PCA)，稀疏按非零tap | H按anchor两侧支持；表输入G4；impulse+非对称核、偶数anchor |
| FIL-02 separable | kx,ky；两遍O(PC(Kh+Kw))，中间场/行缓冲 | 精确只对目标可分离核；逐像素变两遍半径通常不是期望二维核 |
| FIL-03 box sum/mean | width/height≥1；滑动和/前缀O(PC) | sum和mean分开；均匀矩形mean与box共用；与downsample不同 |
| FIL-04 Gaussian | sigma_x/y≥0，radius或truncate；建议新通用版本ceil(4σ)离散归一 | H；σ=0轴identity；当前既有radius1..64/sigma.1..64保持原契约 |
| FIL-05 median/percentile | footprint、q∈[0,1]；偶数样本tie显式 | 小窗排序网络；一般sort/select；定值域hist优化不泛化到任意float |
| FIL-06 bilateral/joint bilateral | spatial_sigma、range_sigma、guide与距离空间、radius必填 | O(PA)参考，H(r)；guide固定时对待滤数据线性，颜色距离不能猜 |
| FIL-07 guided filter | guide+data，radius=r、epsilon>0；局部线性回归 | 盒统计O(P)固定guide维度；标准两次窗口最坏H(2r)；边缘/常量 |
| FIL-08 gradient/Sobel/Scharr | x/y导数、spacing、normalization | signed Gx/Gy；H；linear ramp确认符号和每px单位 |
| FIL-09 magnitude/orientation | Gx,Gy→sqrt(Gx²+Gy²),atan2 | E；zero gradient angle策略；向下y正方向明确 |
| FIL-10 Laplacian | stencil、spacing、scale | signed二阶，4/8邻域具名；常量0、二次函数解析 |
| FIL-11 LoG/DoG | sigma/ratio、scale normalization | LoG与两个Gaussian差不同，后者是尺度近似 |
| FIL-12 Hessian/structure tensor | derivatives/smoothing→components/eigenvalues | derivative halo+outer smooth；方向/脊线/纹理，G3多输出 |
| FIL-13 Canny | single channel，sigma、low<high、L1/L2、connectivity | Gaussian→gradient→NMS→hysteresis，最终W或全局连通协调 |
| FIL-14 Gabor/filter bank | frequency cycles/px、angle、phase、scale、aspect | 实/虚响应，H截断；用于周期纹理/线网分析 |
| FIL-15 Gaussian/Laplacian pyramid | levels、paired down/up kernels→多尺度 | 不同shape输出G3，重建误差与相位明确 |
| FIL-16 unsharp/detail gain | `I+a(I-GI)`，amount=0默认，sigma与threshold | signed残差；negative/HDR需G2，color/luma方式显式 |
| FIL-17 local contrast/local Laplacian | bands、edge threshold、remap | base/detail或特定Local Laplacian；D2，不能把所有Clarity称USM |
| FIL-18 variable box/disk gather | data+radius field→field，每输出位置核、最大radius、有效样本归一 | 动态支持G4；详见bokeh中的gather数学 |
| FIL-19 post antialias | image→image；threshold、search bound、quality | 启发式边缘重建，如SMAA静态子集；D2，与采样预滤波不同 |
| FIL-20 local moments | data→mean/variance/covariance；box sum、square/cross product sum→统计 | H/scan实现；cancellation/大值相减稳定性 |

高斯参考权重为`exp(-x²/(2σx²)-y²/(2σy²))`截断后归一化。大sigma递归、重复box、降采样是具名近似，不能沿用exact标签。连续Gaussian方差相加性质在截断离散核上通常不严格成立；多次圆盘卷积也不是更大均匀圆盘。

Guided Filter的局部系数求解与系数再平均构成两层窗口，2r来自数学依赖上界；同一输入充当guide是一个用法，不应取消独立guide端口。[^guided] Canny的弱边连接可传播任意距离，局部halo无法独立保证全图一致。[^canny]

## 细节与抗锯齿

建议复用`decompose→remap bands→reconstruct`支持锐化、频率分离、去噪和局部对比。Local Laplacian是有专门局部remapping的算法族，应与直接乘Laplacian pyramid系数区分。[^lap]

抗锯齿拆三类：图形生成coverage、重采样前低通、已有图像的后处理。前两类有采样模型，第三类推测原边缘。warp的minification与各向异性见几何章节，bilinear不等于充分低通。[^pbr] SMAA有空间与时域变体，首期只能声明经过实现的静态子集。[^smaa]

## 产品覆盖与使用

Photoshop Custom/Box/Gaussian/Median/High Pass/Unsharp Mask、ACR锐化与亮度/色彩降噪、Lightroom Texture/Clarity、CSP Smoothing可映射到这些基础或组合。官方功能描述并不公开所有核系数、舍入和隐藏处理；规格采用具名数学算法。[^adobe][^csp]

最小链路：`ramp/impulse→kernel→signed output→explicit display normalize`；`image→base+residual→gain→reconstruct`；`guide+mask→guided refine→grade`。颜色边缘以checkerboard背景检查透明黑边，但数值oracle直接检查premul值。

验收必须包括DC、impulse、线性ramp、核大于图、单px、anchor、whole/ROI/tile、导数负值、HDR、小alpha。近似算法另报kernel L1/max误差和高光振铃；性能对比需相同算法语义和质量。Canny加入跨多个tile的长弱边，guided加入距输出r到2r处改变输入的反例。

## 来源

[^opencv]: OpenCV，[*Image Filtering*](https://docs.opencv.org/4.13.0/d4/d86/group__imgproc__filter.html)，4.13.0；correlation、核和边界接口。
[^guided]: He、Sun、Tang，[*Guided Image Filtering*](https://people.csail.mit.edu/kaiming/publications/eccv10guidedfilter.pdf)，ECCV2010，公式5/6/8。
[^canny]: OpenCV，[*Feature Detection*](https://docs.opencv.org/5.0/main_modules/imgproc_feature.html)，访问5.0文档；Canny阈值/连接。
[^lap]: Paris、Hasinoff、Kautz，[*Local Laplacian Filters*](https://people.csail.mit.edu/sparis/publi/2011/siggraph/)，SIGGRAPH2011。
[^pbr]: Pharr等，[*Image Reconstruction*](https://www.pbr-book.org/4ed/Sampling_and_Reconstruction/Image_Reconstruction)，PBRT4，2023。
[^smaa]: Jimenez等，[*SMAA*](https://www.iryoku.com/smaa/)，Eurographics2012。
[^adobe]: Adobe，[*Filter effects reference*](https://helpx.adobe.com/uk/photoshop/using/filter-effects-reference.html)，2024-10-14；[ACR Sharpening](https://helpx.adobe.com/ca/camera-raw/desktop/using/sharpening-noise-reduction-camera-raw.html)，2023-11-03。
[^csp]: CELSYS，[*Filters*](https://help.clip-studio.com/en-us/manual_en/390_filters/Filters.htm)，CSP英文手册；Smoothing等功能。

Texture/Clarity使用面：Adobe，[Lightroom Enhance texture and details](https://helpx.adobe.com/lt/lightroom-cc/how-to/enhance-texture-details.html)，英文官方说明，页面日期未核验。
