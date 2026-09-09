# RAW 与多图摄影流程

状态Proposed，D2。首期支持一个明确的RAW子集与确定性参考算法，其他相机/CFA/压缩类型逐项扩展。RAW解码、文件输出与资产装载由宿主适配提供，算法保持显式输入。LibRaw不是完整Camera Raw显影外观实现。

## 数据和顺序

概念输入是mosaic或多平面数组、CFA pattern/phase、active/data area、orientation、black/white level、linearization与相机校准、曝光信息，以及“已扣黑/已白平衡/已去马赛克”状态。linear RAW、单色、Bayer、X-Trans和其他CFA必须分别声明支持，不能重复应用传感器校正。

典型流程：`decode→linearize→sensor corrections→demosaic（如需要）→camera calibration→scene reference→creative grade→view/output`。具体顺序以来源格式和校准算法为准；lens shading、畸变、横向色差可共用profile而独立计算。裁切必须更新CFA相位。

| ID / 算子族 | 输入 → 输出 | 算法与关键参数 | 验收 |
| --- | --- | --- | --- |
| RAW-01 decode adapter | accepted encoded source→planes+metadata | decoder版本、格式子集、orientation和原始位深；G6 | 尺寸/原始值/metadata和重复解码一致；非法数据有界失败 |
| RAW-02 linearize | sensor+LUT→linear sensor | LUT和domain来自数据，不自动用min/max拉伸；E+表 | LUT端点与非线性已知点 |
| RAW-03 normalize | sensor+black/white model→linear+saturation mask | generic radiometric与dng_reference分别命名 | 行列偏置、通道白电平、负值与饱和 |
| RAW-04 defect correct | sensor+defect list/检测→sensor+mask | 同CFA通道的邻域插值；检测阈值/半径具名 | 边界/坏点簇；星点不能被无条件去除 |
| RAW-05 dark/flat calibration | sensor+dark/flat+exposure metadata→sensor+validity | 暗场匹配，平场除法和近零策略；E | 均匀场复原、无效校准区明确 |
| RAW-06 demosaic | mosaic+CFA→camera RGB | Bayer bilinear作为可检查基线；高质量/学习算法独立mode | 常量色块、斜边/细纹、crop相位、边缘 |
| RAW-07 WB gains | camera samples+gains→camera samples | per-channel gains，归一方式必填；already-applied状态 | 灰卡中性、未饱和数据线性；不重复WB |
| RAW-08 camera calibration | RGB+matrices/profile+illuminant→scene color | colorimetric matrix、双光源插值、look/tone分开 | 指定参考色块/白点；颜色状态可追溯 |
| RAW-09 highlight reconstruction | RGB+saturation mask→RGB+confidence | 通道/空间/模型重建具名；method和最大邻域 | 未饱和区域保持，全通道clip不声称恢复真色 |
| RAW-10 lens correction | image+calibration→image+validity | shading、distortion、lateral chromatic aberration拆分 | 标定网格、均匀场、颜色边缘对齐 |
| RAW-11 gain-map HDR | base image+gain map+metadata→HDR/SDR | 输入格式具名，map重采样、gain范围/显示条件 | metadata端点；不是任意“曝光图乘RGB” |
| RAW-12 render workflow | 上述结果+显影配置→scene/display image | 顺序、参数、Process Version或自有版本显式 | 固定recipe可复现；不混同解码器默认渲染 |

DNG1.7.1.0的参考归一化分母使用WhiteLevel减去sample plane的maximum computed black level，不能把通用逐像素`(raw-B(x,y))/(white-B(x,y))`标为DNG一致。其早期处理允许保留负值。[DNG p.99](https://helpx.adobe.com/content/dam/help/en/photoshop/pdf/DNG_Spec_1_7_1_0.pdf)。LibRaw的black与`as_shot_wb_applied`等状态必须进入适配判断。[LibRaw数据结构](https://www.libraw.org/node/31)

## 多图摄影

输入为显式图像列表、各图时间/曝光/有效mask、可选校准与变换。多图不同data window要先对齐到共同坐标。输出confidence、source map、focus index是数值诊断，当前单Value输出需G3。

| ID / 功能 | 输入 → 输出 | 实现路线/参数 | 验收与边界 |
| --- | --- | --- | --- |
| PHO-01 register images | reference+moving+mask→transform/inliers | translation/affine/homography；feature+RANSAC或ECC具名；iterations/tolerance | 已知变换、低纹理失败、遮挡/重复纹理 |
| PHO-02 stack reduce | aligned images+weights→mean/median/min/max/variance | 有效样本、trim比例、sigma-clipping迭代显式；Float64累加建议 | 异常帧、全无效区、权重归一 |
| PHO-03 HDR merge | exposures+exposure values→relative_radiance+validity | 线性输入曝光归一，响应未知时估计；参考曝光/全局尺度、weights/rejection | 曝光倍率2恢复同相对radiance，饱和mask；有物理校准及单位才声明绝对radiance |
| PHO-04 exposure fusion | LDR exposure stack→fused image | contrast/saturation/well-exposed权重→multiscale blend | 权重和1，接缝/halo；不声称物理radiance |
| PHO-05 deghost | aligned stack+reference→weights/ghost mask | 运动证据和源选择分离；阈值/参考帧显式 | 动态物体不重复，静态区域不误删 |
| PHO-06 focus stack | focal stack→image+source index/confidence | focus measure→正则选择→multiscale融合 | 前后轮廓、半透明、反射、呼吸效应；index不是米制深度 |
| PHO-07 panorama | images+camera/projection→panorama+source/validity | match→global optimize→projection→exposure→seam→multiband | 闭环接缝、地平线、近景视差与空洞 |
| PHO-08 burst/pixel shift | samples+phase/motion→denoise/superres+confidence | 采样模型、配准、遮挡、重建；D3 | 未对齐平均不等于超分；moving与static分开 |

HDR、全景和景深合成在产品中分别包含对齐、去鬼影、投影和融合选择，适合组织成可检查中间结果的workflow。[^lr][^focus] Debevec–Malik研究响应恢复与radiance；Mertens exposure fusion提供不同目标，二者应保留不同输出语义。[^hdr][^fusion]

仅有曝光比时HDR重建存在全局尺度不确定性，默认输出相对辐射度；绝对物理亮度需要额外辐射度校准，不能只凭曝光metadata推定。

## CPU/GPU、Region与资源

sensor局部校正可H/E，demosaic按算法支持半径，色彩校准E。配准、HDR响应估计、全景优化和无界图像列表暂W；全尺寸多图直接堆入内存不一定可行，必须给源分块、pyramid与临时磁盘的宿主策略。当前内核没有由本规格自动增加多图/时间执行能力。

Float32/64是中间数据建议，UInt16等原始容器支持需G1；原始负值与色变换negative需G2。参考CPU实现不允许缺省自动tone curve改变输入物理意义。GPU demosaic/warp/fusion需对应oracle，不能只验证文件能打开。

W11提供最小RAW/HDR链路。额外采用synthetic CFA常量和高对比斜边、不同曝光的同一线性场、明确mask的坏点和暗场作为可复现测试；真实相机样本另记录许可与metadata。

## 来源

[^lr]: Adobe，[*HDR photo merge*](https://helpx.adobe.com/lightroom-classic/desktop/process-and-develop-photos/hdr-photo-merge.html)、[*Panorama*](https://helpx.adobe.com/lightroom-classic/desktop/process-and-develop-photos/panorama.html)，滚动Lightroom Classic指南。
[^focus]: Adobe，[*Create a composite with extended depth of field*](https://helpx.adobe.com/photoshop/desktop/create-masks/blend-images/create-a-composite-with-extended-depth-of-field.html)，2026-02-23。
[^hdr]: Debevec、Malik，[*Recovering High Dynamic Range Radiance Maps from Photographs*](https://people.eecs.berkeley.edu/~malik/papers/debevec-malik97.pdf)，SIGGRAPH1997，作者论文。
[^fusion]: Mertens、Kautz、Van Reeth，[*Exposure Fusion: A Simple and Practical Alternative to High Dynamic Range Photography*](https://onlinelibrary.wiley.com/doi/10.1111/j.1467-8659.2008.01171.x)，2009。

补充规范：Adobe [DNG1.7.1.0](https://helpx.adobe.com/content/dam/help/en/photoshop/pdf/DNG_Spec_1_7_1_0.pdf)，2023-09；[SDK入口](https://helpx.adobe.com/camera-raw/desktop/dng-and-file-formats/digital-negative.html)访问时为1.7.1 Build2724，2026-09-08。规范版本与SDK build不同；首期不承诺全部DNG tag/opcode或私有RAW压缩。
