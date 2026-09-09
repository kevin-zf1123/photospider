# 原始需求逐项映射

本表保留需求覆盖关系。名称相近的功能只有数学语义相同时才合并；拆分表示需要多个可复用组件，并不表示缺失。详细支持范围、算法和验收见对应分类页。

## 数值、格式、生成与选区

| 原始项 | 规格位置 | 整理结果 |
| --- | --- | --- |
| 一维表达式、采样间隔生成数组 | [数值](../01-numeric/core.md) NUM-01 | 增加 domain/count/端点与单位；表达式使用有界纯 AST |
| 单通道控制点曲线 | [曲线](../01-numeric/curves.md) | 控制点插值与采样分开 |
| 三通道控制点曲线 | [曲线](../01-numeric/curves.md) | `[N,3]`；另列真正三维 LUT |
| 通道 extract | [格式](../02-format-color/representation.md) | 补 channel merge/append/reorder/swizzle/replace |
| alpha 转换 | [格式](../02-format-color/representation.md) | straight/premultiplied/opaque；alpha=0 与隐含颜色策略 |
| 色域与校色文件 | [格式](../02-format-color/representation.md) | 原色/白点变换与 ICC/profile pipeline，指定 intent |
| Lab/CMYK/RGBA/RGB/灰度/黑白/LCh/HSL/HSB/OKLCh/YCbCr | [格式](../02-format-color/representation.md) | 模型转换、profile、transfer、量化分解；HSB 通常是 HSV 别名，仍需绑定公式 |
| 数值类型与 [0,1]/[0,255]/[0,65535] | [格式](../02-format-color/representation.md) | uint8/uint16 的无符号范围；signed int8/int16 不具有相同范围 |
| 选区生成、图形 | [蒙版](../04-mask-morphology/masks.md)、[生成](../03-generation/generators.md) | 几何 coverage、颜色阈值、连通填充、路径 mask、外部模型 mask |
| Bézier 控制点/粗细/采样 | [路径](../03-generation/paths.md) | path topology、sample path、width profile、stroke rasterize 分开；优先弧长参数 |
| 每通道一维数组生成渐变 | [生成](../03-generation/generators.md) | 还需二维 scalar coordinate field；LUT lookup 后颜色转换 |
| 非整数放大/缩小、方形/圆形 | [蒙版](../04-mask-morphology/masks.md) | metric + 距离/coverage 定义；二值几何与灰度形态学分开 |
| and/or/xor/not | [蒙版](../04-mask-morphology/masks.md) | 二值逻辑、fuzzy min/max、概率 coverage 不能混为一套 |
| 羽化 | [蒙版](../04-mask-morphology/masks.md) | Gaussian 与 SDF 边界过渡分别命名 |
| Perlin/Gaussian/Voronoi | [生成](../03-generation/generators.md) | gradient noise / 分布 / cellular noise 分开；补 white/blue/fractal/tileable |

## 滤镜与调色

| 原始项 | 规格位置 | 整理结果 |
| --- | --- | --- |
| box、mean | [空间滤镜](../05-filter/spatial.md) | 均匀矩形算术平均可共用 box；任意窗口 mean 更一般；与缩小 box 区分 |
| median、Gaussian | [空间滤镜](../05-filter/spatial.md) | 秩滤波与线性滤波不同；当前 RGBA Gaussian 保留 |
| lens bokeh、波长 | [散景](../06-optics/bokeh.md) | 固定 PSF、可变 gather、源 scatter、遮挡、衍射/光谱分层 |
| Sobel x/y、gradient、Laplacian、Canny | [空间滤镜](../05-filter/spatial.md) | signed derivative、梯度组合、二阶、全局滞后连接 |
| FFT、自定义卷积核 | [频域](../05-filter/frequency-restoration.md)、[空间](../05-filter/spatial.md) | FFT/iFFT/complex乘法是基础；convolution 与 correlation 指定核翻转 |
| 抗锯齿 | [路径](../03-generation/paths.md)、[几何](../08-transform/geometry.md)、[空间](../05-filter/spatial.md) | 生成 coverage、缩小时预滤波、已有图像后处理三类 |
| 锐化、高通、低通 | [空间](../05-filter/spatial.md)、[频域](../05-filter/frequency-restoration.md) | unsharp 与频率 mask；边缘恢复另列反卷积 |
| HSL、LCh、Lab | [调色](../07-grade/adjustments.md) | 模型定义与控制方式分开，明确 achromatic hue 和 gamut |
| 曲线、渐变映射、LUT | [调色](../07-grade/adjustments.md)、[曲线](../01-numeric/curves.md) | LUT 生成/应用、标量导出、颜色插值、3D耦合映射 |
| 色阶、白平衡 | [调色](../07-grade/adjustments.md) | levels 与 camera WB/chromatic adaptation 分开 |
| 黑白 | [调色](../07-grade/adjustments.md) | 提取 L*、线性 luminance、可调混色 monochrome 独立 |
| 色调分离（原清单两次） | [调色](../07-grade/adjustments.md) | 合并 posterize；补独立 threshold/dither/halftone |
| 曝光 | [调色](../07-grade/adjustments.md) | EV 转 gain=2^EV；已有 gain op 的输入范围不可忽略 |
| 高光/暗部，阈值/影响范围 | [调色](../07-grade/adjustments.md) | tonal-zone 与含邻域的局部 tone mapping 分开 |
| 通道混合、色彩平衡、反转 | [调色](../07-grade/adjustments.md) | 矩阵、tone-zone偏移与显式pivot反转 |
| Hue-vs-Hue/Luma/Sat、Sat-vs-Luma 等 | [调色](../07-grade/adjustments.md) | 通用特征提取→周期/非周期曲线→目标分量修改；支持 luma-vs-sat、sat-vs-sat |

## 变换、混合与分析

| 原始项 | 规格位置 | 整理结果 |
| --- | --- | --- |
| 仿射 | [几何](../08-transform/geometry.md) | 补 resize/crop/pad/reformat/homography，统一 inverse sampler |
| 置换图、液化 | [几何](../08-transform/geometry.md) | displacement→STMap→resample；笔刷编辑场与采样分开 |
| 鱼眼和光栅化对话 | [投影](../08-transform/projections.md) | 2D 相机模型转换与 3D 射线/曲边光栅化分别界定 |
| 加减乘除、比较明暗、滤色 | [混合](../09-composite/blending.md) | 数值计算与图层合成 wrapper 分开；比较逐通道或标量 |
| 加深/线性加深/加亮/发光版本 | [混合](../09-composite/blending.md) | color burn、linear burn、dodge、plus、glow 的公式/alpha 分开 |
| overlay/softlight/hardlight/linear/vivid/point light | [混合](../09-composite/blending.md) | point light 暂映射为常用 pin light 名称，原软件含义待核验 |
| 差的绝对值 | [混合](../09-composite/blending.md) | difference=abs(B-S)，补 exclusion、signed subtract |
| 色相/彩度/颜色/明度 | [混合](../09-composite/blending.md) | 规范 Lab/LCh 默认方案，legacy SetLum/SetSat 单列；不宣称软件内部 YCbCr |
| 溶解 | [混合](../09-composite/blending.md) | 显式 seed/坐标、概率 coverage 与静态抖动；时域策略独立 |
| 直方图/频谱图/波形图/PSD/矢量示波器 | [分析](../10-analysis/scopes.md) | 原始统计与渲染分离；轴、空间、编码范围、窗口、归一化显式 |

## 额外补齐的基础

新增需求包括：坐标网格与 identity STMap、channel merge/swizzle、数据类型与颜色标签检查、resample 与重建核、归约/prefix sum、连通域与距离变换、局部边缘保持、去噪与反卷积、keyer/despill/matte refine、clone/heal/inpaint、RAW 前处理/去马赛克、曝光融合/HDR/堆栈/全景、ΔE/差图/false color/gamut warning、时间/光流/跟踪、AOV/deep 与模型适配。

完整性边界：软件资产管理、绘画工作区、笔刷素材商店、排版排版引擎、3D 场景编辑、音频、剪辑 UI、协作/云服务不是本算子规格的交付对象。对图像处理有直接影响的字体光栅化、时间采样、模型输出、3D 数据输入则在对应后期能力中保留。
