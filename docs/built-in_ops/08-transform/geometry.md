# 几何变换、重采样与液化

状态Proposed。基础inverse sampler、affine与map合成为D1；非线性网格、逆解与高质量footprint为D2。向量场为Float32/64 `[Hout,Wout,2]`，不是颜色图像。默认像素边界坐标，中心 `(x+.5,y+.5)`；向量分量x,y，数组y,x,c。

核心为`Iout(p)=sample(Iin,M(p))`，M从输出坐标映到输入。OpenCV公开几何变换也以反向取样区分插值与域外扩展；接入其整数像素中心坐标需要±.5适配。[^opencv]

## 算子目录

| ID / 功能 | 输入 → 输出 | 参数与实现 | 验收 |
| --- | --- | --- | --- |
| GEO-01 crop/pad/reformat | image+canvas→image | integer bounds、fit/fill/stretch、pixel aspect；outside=transparent建议 | crop不等于resize；原点/data window与颜色不变 |
| GEO-02 flip/transpose/rotate90 | image→排列图 | 轴/方向明确、精确copy/view | 整数元素完全保持，奇偶维度 |
| GEO-03 resize | image+size→image | center convention、filter、antialias=true建议 | 常量、identity、非整数比例、缩小zone plate |
| GEO-04 map affine | matrix2×3/3×3→map或应用 | input→output还是inverse显式，pivot/rotation/shear顺序 | singular reject、已知点、identity |
| GEO-05 homography/corner pin | H或四点对应→map | denominator sign/zero与有效域 | 退化点组、无穷远穿越、透视校正 |
| GEO-06 remap/STMap | image+inverse map→image+validity | pixel/normalized、filter、boundary、map alpha decode | zero displacement identity，方向，input/output不同尺寸 |
| GEO-07 displacement decode | encoded field→pixel vectors | scale、offset、axis、forward/inverse必填 | .5中性仅是编码约定，signed field直接0中性 |
| GEO-08 compose maps | M1,M2→M1∘M2 | 先后顺序、map采样核、validity传播 | 非常量场反例，map连续而图像只采样一次 |
| GEO-09 invert map | forward map→inverse+validity | max iterations、residual tolerance、fold policy | 多对一/空洞/不收敛不能伪造inverse |
| GEO-10 grid/FFD/TPS/MLS warp | control constraints→map | basis、boundary pins、regularization具名 | 控制点命中、局部/全局影响、折叠 |
| GEO-11 liquify brush | old map+ordered strokes+freeze mask→map | push/twirl/pucker/bloat/reconstruct，radius/pressure/spacing | stroke顺序、冻结场、重建区域、累计不反复采图 |
| GEO-12 polar/log-polar | center+scale→map | radius域、angular seam、log base | r=0奇点，域外和inverse |
| GEO-13 lens distort/undistort | calibration+camera→map | 具名模型、valid FOV、inverse convergence | 网格重投影、畸变边缘、有效mask |
| GEO-14 reproject/panorama | source/target projection→map | orientation、FOV、chart、pixel aspect | seam/cubemap面边界、未知视角无数据 |
| GEO-15 mip pyramid/sample footprint | image+map/Jacobian→sampled | reconstruction、max anisotropy、lod规则 | minification与各向异性，质量档误差 |

首版默认图像sampler可采用bilinear+域外transparent，resize缩小时必须明确低通；标签/ID采用nearest，normal插值后按语义归一化，flow按数据向量处理。当前profile已支持有限非负HDR；signed RGB、其他颜色语义与可能产生负值的负瓣核输出需要G2，不能隐式clip掉负值后声称滤波正确。

## 重采样方法

| 方法 | 成本与用途 | 需要固定的语义 |
| --- | --- | --- |
| nearest | 单样本，标签/像素艺术 | halfway tie、坐标原点 |
| bilinear | 2×2重建 | 适合放大/小变化；大幅缩小不足以低通 |
| bicubic | 常见4×4 | B/C或a参数、支撑和负瓣；“cubic”不是唯一核 |
| area/box | 覆盖区域平均 | 对缩小像素面积积分，边缘分母 |
| Lanczos | sinc窗，support=2/3等显式 | ringing、negative与alpha超界策略 |
| EWA | Jacobian椭圆footprint，mip可选 | anisotropy cap改变模糊，核/LOD/质量档固定 |

`J=∂M/∂p`决定输出像素在源图中的footprint，缩小方向必须预滤波。EWA与mip路线可参考PBRT，但导数质量、最大各向异性和边界策略必须成为本op参数。[^pbr] 颜色图像在指定线性premul空间重采样，非线性色彩空间/straight边缘先显式转换；data map不做gamma或ICC。

## 液化的映射合成

若M1(p)=p+d1(p)，M2(p)=p+d2(p)，依次重采样的整体映射为M1(M2(p))，总位移是 `d2(p)+d1(p+d2(p))`，一般不是d1(p)+d2(p)。

反例：p=(.2,.3)、d1(x,y)=(x,0)、d2=(1,0)，正确总位移(2.2,0)，直接相加(1.2,0)。保留原始图像与累计map，最后采样一次以减少图像反复重建损失；map自身的插值误差仍需测量。

冻结mask控制编辑是否改变map，与最终`mix(original,warped,mask)`是两种不同语义。brush按固定空间spacing积分，pressure/density/flow、重建强度与撤销由宿主传入有序数据。Adobe公开Liquify工具名称和控件，内部核未公开；数学版采用具名radial kernel或MLS/FFD，不声称兼容。[^liquify][^mls]

## STMap互操作

Nuke官方STMap以归一化U/V表示绝对源位置，左下(0,0)、右上(1,1)，并有UV premultiplied选项。[^nuke] 应提供具名`decode_nuke_st`适配与测试；精确半像素换算需真实fixture确认。图中颜色编码外观不改变它是数据图的事实。

## Region、资源与验收

affine可映射矩形顶点后扩大filter footprint；homography须排除分母零点；任意非线性映射有内部极值、缝和跨面采样，只看四角不保证输入bbox保守。某tile指向全源图时真实需求可以很大。当前只有Shrink专用映射，其他先满足shape/port后使用Whole或提议新的G4规则，不存在自动warp Region能力。

验收包括整数排列bit保持、identity map、已知点、map合成反例、bilinear解析2×2、常量保持、zero/negative scale、极点、odd尺寸、alpha边缘与zone plate缩小频谱。Whole/ROI/tile一致与前向dirty传播分别检查；局部编辑map不总意味着只读局部源。

典型链路：`grid→noise/brush deformation→compose→remap original→validity→composite`；`lens calibration→undistort map→复用多个帧/图像`。

## 来源

[^opencv]: OpenCV，[*Geometric Image Transformations*](https://docs.opencv.org/4.13.0/da/d54/group__imgproc__transform.html)，4.13.0。
[^pbr]: Pharr、Jakob、Humphreys，[*Image Texture*](https://www.pbr-book.org/4ed/Textures_and_Materials/Image_Texture)，PBRT4，2023。
[^liquify]: Adobe，[*Overview of Liquify filter*](https://helpx.adobe.com/photoshop/desktop/effects-filters/artistic-stylize-filters/overview-of-liquify-filter.html)，2026-02-23。
[^mls]: Schaefer、McPhail、Warren，[*Image Deformation Using Moving Least Squares*](https://people.engr.tamu.edu/schaefer/research/mls.pdf)，SIGGRAPH2006。
[^nuke]: Foundry，[*STMap*](https://learn.foundry.com/nuke/content/reference_guide/transform_nodes/stmap.html)，滚动参考；[SplineWarp](https://learn.foundry.com/nuke/content/reference_guide/transform_nodes/splinewarp.html)记录不同warp版本的影响范围差异。
