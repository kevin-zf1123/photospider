# Source references

Imported reference inventory from the source archive. Dates describe that inventory,
not new verification or backend/resource approval. Accepted decisions and current
member specifications govern behavior. Old oracle-file claims are historical only.

# 03-generation：资料、采用边界与检索记录

检索日期：2026-09-25。只采用标准组织、作者、官方实现和项目官方文档。
下列事实来源不自动成为 Photospider 规范；数学舍入继承 NUM，颜色含义继承 FMT。
未引入或执行Random123/CGAL/Clipper2/NVIDIA等外部生产后端；滚动页面不是版本pin。
独立oracle使用的预装mpmath/NumPy版本见验证报告。规范需要的常量和有限测试向量已写入本地。

## S01

**W3C SVG 2 Paint Servers**。2018-10-04 CR / 本次访问 2026-09-25。

来源：<https://www.w3.org/TR/SVG2/pservers.html>

采用边界：坐标系、spread、线性和径向渐变。仅借用词汇与几何；退化、采样和舍入以本草稿为准。

## S02

**W3C SVG 2 Paths**。2018-10-04 CR / 访问 2026-09-25。

来源：<https://www.w3.org/TR/SVG2/paths.html>

采用边界：M/L/Q/C/Z、子路径和闭合语义；不是 SVG 文本解析器或完整兼容承诺。

## S03

**W3C SVG 2 Painting**。2018-10-04 CR / 访问 2026-09-25。

来源：<https://www.w3.org/TR/SVG2/painting.html>

采用边界：nonzero/evenodd、cap/join/miter/dash；Photospider 的精度与零长策略单独定义。

## S04

**W3C CSS Color 4**。2026-09-13 固定 CRD / 访问 2026-09-25。

来源：<https://www.w3.org/TR/2026/CRD-css-color-4-20260913/>

采用边界：插值空间、色相和 alpha 的语义来源；具体模型计算复用仓库 CRV/FMT，不采用浏览器默认行为。

## S05

**D. E. Shaw Research Random123**。官方实现 / 访问 2026-09-25。

来源：<https://github.com/DEShawResearch/random123>

采用边界：counter-based 方法；本草稿独立固定 counter 编码，非上游 API 默认编码。

## S06

**Random123 philox.h**。main 浮动页 / 常量与轮数在本地冻结。

来源：<https://raw.githubusercontent.com/DEShawResearch/random123/main/include/Random123/philox.h>

采用边界：Philox4x32-10 的乘数、Weyl 常量与置换；没有下载或捆绑其 C/C++ 实现。

## S07

**Random123 known-answer vectors**。main 浮动页 / 本地保留 3 个 Philox4x32-10 数值向量。

来源：<https://raw.githubusercontent.com/DEShawResearch/random123/main/tests/kat_vectors>

采用边界：提供独立外部已知答案，不能用自生成 golden 代替。

## S08

**Ken Perlin Improved Noise**。2002 作者参考 / 访问 2026-09-25。

来源：<https://mrl.cs.nyu.edu/~perlin/noise/>

采用边界：固定排列、梯度选择与五次 fade。新 seed/hash 必须另具名。未复制 Java 源码。

## S09

**Robert Bridson Fast Poisson Disk Sampling**。SIGGRAPH 2007 sketch，已核阅单页 PDF。

来源：<https://www.cs.ubc.ca/~rbridson/docs/bridson-siggraph07-poissondisk.pdf>

采用边界：active list、annulus 候选与邻域加速；本项目额外固定遍历、随机槽和停止规则。

## S10

**Wolfe 等 Spatiotemporal Blue Noise Masks**。EGSR 2022 / 访问 2026-09-25。

来源：<https://research.nvidia.com/publication/2022-07_spatiotemporal-blue-noise-masks>

采用边界：二维蓝噪声与时空频谱不是同一保证。

## S11

**NVIDIA STBN SDK**。官方仓库访问 2026-09-25；本次未采用任何资源包。

来源：<https://github.com/NVIDIA-RTX/STBN>

采用边界：资源与生成工具的候选来源；release、内容哈希、许可和质量阈值需另行批准。

## S12

**PBRT 4 Sampling 1D Functions**。第4版 A.4 / 访问 2026-09-25。

来源：<https://www.pbr-book.org/4ed/Sampling_Algorithms/Sampling_1D_Functions>

采用边界：逆分布和 Box–Muller 方法背景；本草稿以数学公式而非网页代码为权威。

## S13

**PBRT 3 Noise**。第3版 Noise / 访问 2026-09-25。

来源：<https://www.pbr-book.org/3ed-2018/Texture/Noise>

采用边界：多 octave 与采样足迹的关系；频率截断不等于严格带限。

## S14

**Jonathan Shewchuk Robust Predicates**。作者页面 / 访问 2026-09-25。

来源：<https://www.cs.cmu.edu/~quake/robust.html>

采用边界：几何谓词应独立于普通近似浮点；本草稿不捆绑 predicates 源码。

## S15

**CGAL 2D Regularized Boolean Set-Operations**。官方滚动手册 / 访问 2026-09-25。

来源：<https://doc.cgal.org/latest/Boolean_set_operations_2/index.html>

采用边界：regularized 集合布尔词汇与精确几何后端候选；不宣称当前已采用 CGAL。

## S16

**Angus Johnson Clipper2 Overview**。页面标注 2.0.0 / 访问 2026-09-25。

来源：<https://angusj.com/clipper2/Docs/Overview.htm>

采用边界：多填充规则、polygon offset 的候选实现；坐标量化不能冒充原浮点几何的精确解。

## S17

**SciPy BSpline**。官方手册 / 访问 2026-09-25。

来源：<https://docs.scipy.org/doc/scipy/reference/generated/scipy.interpolate.BSpline.html>

采用边界：Cox–de Boor 基函数递推和有效 knot 区间；禁用隐式外推。

## S18

**SciPy PchipInterpolator**。官方手册 / 访问 2026-09-25。

来源：<https://docs.scipy.org/doc/scipy/reference/generated/scipy.interpolate.PchipInterpolator.html>

采用边界：形状保持插值背景；实际斜率与舍入边界继承 CRV-01B。

## S19

**NumPy Generator.poisson**。官方手册 / 访问 2026-09-25。

来源：<https://numpy.org/doc/stable/reference/random/generated/numpy.random.Generator.poisson.html>

采用边界：Poisson PMF 及 count 输出；不把 NumPy seed 序列定义为本项目序列。

## S20

**Philip J. Schneider FitCurves**。Graphics Gems 作者代码存档 / 访问 2026-09-25。

来源：<https://github.com/erich666/GraphicsGems/blob/master/gems/FitCurves.c>

采用边界：三次拟合的算法背景；离散残差不能替代连续 Hausdorff 证书。

## S21

**EMVA 1288**。标准组织入口 / 访问 2026-09-25。

来源：<https://www.emva.org/standards-technology/emva-1288/>

采用边界：传感器表征背景。未逐条验证完整标准，因此不作 EMVA 合规或相机物理标定声明。

## S22

**GIMP RGB Noise**。GIMP 3.0 官方手册 / 访问 2026-09-25。

来源：<https://docs.gimp.org/3.0/en/gimp-filter-noise-rgb.html>

采用边界：仅确认艺术性通道噪声功能，不建立任何商业软件 bit identity。

## 检索排除

初次通用搜索出现不相关结果，未采用。GNU GSL integration 与 Basler 页面未成功读取，未作为证据。未进行商业产品黑盒比对。所有“建议默认”“资源上限”“算法编码”“几何质量门槛”均为本项目草稿选择，不伪称来自标准。
