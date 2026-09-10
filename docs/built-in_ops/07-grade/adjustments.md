# 调色与局部色调

2026-09-11：本轮基础子集的接口、Region 与可运行示例见[基础算子实现](../../kernel-architecture/zh/Basic-Operations.zh.md)。其他目录项继续保持原研究状态。

状态 Proposed；逐像素明确公式为D1，局部tone mapping、复杂色域压缩和商业控制匹配为D2。输入为描述明确的RGB/Lab等图像，可选mask；默认保留alpha。参考使用Float32颜色、Float64系数与统计，GPU FP32须单独验证。E/H/W为数学依赖；辅助LUT/统计表的完整输入需求仍需G4。

## 调色目录

| ID / 提议操作 | 输入输出与关键参数 | 数学/算法与相近功能 | 最小验收 |
| --- | --- | --- | --- |
| GRD-01 `grade.exposure` | linear RGB→RGB；EV=0，有限 | gain=2^EV；alpha保留；当前gain op最多16，包装范围须相容或新定义；E | .18,+1EV→.36，0EV identity |
| GRD-02 `grade.offset_gain` | RGB+offset/gain；0/1 | affine `out=in*gain+offset`，不默认clip；Lift/Gamma/Gain独立mode；E | 0/1 identity、negative合法输出需G2 |
| GRD-03 `grade.contrast` | RGB或Luma；contrast=1,pivot=.18线性建议 | `(x-p)*c+p`是明确线性版；log contrast另用log曝光轴；E | pivot不动，c=1 identity |
| GRD-04 `grade.levels` | channel/feature→同形；black=0,white=1,gamma=1,out0=0,out1=1 | 归一化后按显式clip与幂律映射；避免black=white；E | black/white映到out0/out1，中点幂律 |
| GRD-05 `grade.curves` | RGB+`[N,1或3]`→RGB | per-channel或master曲线，master位置/亮度特征明确；linear/PCHIP；E+whole表 | identity表；W1离散查表值 |
| GRD-06 `grade.gradient_map` | scalar feature+color ramp→RGB | 从Y/L*/max等取t，再ramp查表；不是3D LUT；E | t=0/1端点，alpha策略明确 |
| GRD-07 `grade.lut3d` | RGB+`[Nr,Ng,Nb,3]`→RGB | trilinear/tetrahedral；建议tetrahedral，domain和shaper显式；E+表 | identity、可交换RGB通道的非可分离测试 |
| GRD-08 `grade.hsl` / `hsv` | 指定RGB域→同域；Δh=0°,sat=1,lightness=0 | 模型转换后修改；HSL L=(max+min)/2，HSV V=max；E | hue周期360，灰轴处理、无变化identity |
| GRD-09 `grade.lch` / `oklch` | common-white RGB→RGB；ΔL=0,Cscale=1,Δh=0 | 先明确转换，再修改L/C/h，gamut策略独立；E | 只改h时目标空间L/C保持至gamut前 |
| GRD-10 `grade.lab` | Lab→Lab；Lscale=1,a/b offset=0 | 独立修改L*,a*,b*；与HSL不共享“饱和度”标度；E | neutral轴偏移产生预期色向 |
| GRD-11 `grade.white_balance` | linear camera/working RGB→RGB；source/target white必填 | camera gains或chromatic adaptation，temperature/tint须指定映射；E | 单位gains/同白点identity、灰卡中性；是否已应用WB依赖宿主处理阶段，不能仅从像素推断 |
| GRD-12 `grade.monochrome` | RGB→gray或同灰RGB | modes: linear_Y、Lab_L、channel_weights；weights默认用空间Y矩阵行 | Lab L*数值不能原样当linear灰；w和是否归一显式 |
| GRD-13 `grade.posterize` | feature/channel→同形；levels=8,levels≥2 | 建议`round(x*(n-1))/(n-1)`，域[0,1]；clip/round显式；E | ties-even时n=2、x=.5输出0，不能与>=.5阈值混同 |
| GRD-14 `grade.threshold` | scalar→binary mask；t=.5 | `x>=t`，硬边；局部adaptive threshold另H/W | t端点归属明确 |
| GRD-15 `grade.channel_mixer` | RGB/C通道→目标C | matrix+bias，默认identity；monochrome可输出1通道；E | swap R/B、row weights已知结果 |
| GRD-16 `grade.color_balance` | RGB→RGB；zone offsets=0 | 用明确luminance mask对阴影/中间调/高光作色向偏移；preserve L*独立选项 | 三zone权重分区/归一和neutral无变化 |
| GRD-17 `grade.invert` | channel→同形；pivot range [0,1] | `out=lo+hi-in`；负片和物理反射率模型另列；E | 反转两次identity；HDR>1变负不可夹掉 |
| GRD-18 `grade.tonal_zones` | RGB+threshold/width→RGB | 全局标量特征的soft masks，调整每区gain/offset；E | 门限两侧连续，零strength identity |
| GRD-19 `grade.shadow_highlight_local` | RGB→RGB；radius/sigma,thresholds,strength | edge-aware base/detail decomposition + base压缩；H/W | halos、纹理保持与局部contrast；不声称PS公式 |
| GRD-20 `grade.feature_curve` | RGB+curve→RGB；source/target feature必填 | hue/luma/sat/chroma等source取曲线，再改目标；周期source与非周期分开 | hue首尾连续、灰色稳定、负delta可解释 |
| GRD-21 `grade.saturation` / `vibrance` | RGB→RGB；strength=0或scale=1 | saturation明确模型；vibrance按已有chroma抑制的非线性增益，算法版本化 | 灰轴不变、skin保护若有需指定mask而非猜测 |
| GRD-22 `grade.split_tone` / `color_wheels` | RGB→RGB；zone hues/chromas/strength | tonal masks + chroma染色；offset/lift/log/HDR wheels各自参数化 | 0strength identity；zone交叠边界稳定 |
| GRD-23 `grade.selective_color` | RGB→RGB；color-region weights + deltas | hue/chroma/luminance软选择或明确CMYK校正模型 | Adobe“Selective Color”精确兼容需单独证据 |
| GRD-24 `grade.auto_levels` / `auto_exposure` | RGB+percentiles/target→RGB | stats→参数→grade的组合；默认显式ROI与ignore透明策略；W+E | 饱和/空mask保护，统计和应用可分别检查 |
| GRD-25 `grade.histogram_equalize` / `clahe` | scalar feature→feature | CDF全局映射 / tile直方图clip+插值；W或邻tile协调 | 常量图不生伪纹理；clip limit定义与分块接缝 |
| GRD-26 `grade.vignette` | RGB+coordinate field→RGB | 椭圆距离→soft profile→曝光；center/roundness/feather明确 | center增益、角点增益解析；可组合mask实现 |
| GRD-27 `grade.asc_cdl` | RGB→RGB；slope=1,offset=0,power=1,saturation=1 | 按CLF指定Fwd/Rev/FwdNoClamp/RevNoClamp与顺序；E | identity、负值/逆分支；FwdNoClamp的负slope-offset值跳过power，不冒充signed-power |
| GRD-28 `grade.color_warper` | RGB+颜色域二维网格`[Nu,Nv,2]`→RGB | hue/chroma或chroma/lightness网格位移；basis、hue周期、边界pins显式；D2 | identity网格、控制点、折叠/过冲；不同于一维feature_curve和已有3D LUT应用 |

曲线/LUT分离参照CLF的1D/3D与处理节点语义。[^clf] Adobe官方文档确认Levels/Curves、Shadow/Highlight的Radius/Tonal Width等控制存在，不能由控件名称推出其内部公式。[^levels][^shadow] Resolve官方Color页提供curves、qualifiers、HDR工具等功能锚点，具体重现仍以本篇公式或另行黑盒证据为准。[^resolve]

## 关键数学与默认选择

Levels规范候选：`u=(x-black)/(white-black)`；bounded模式先clip到[0,1]，`y=out0+(out1-out0)*u^(1/gamma)`，gamma>0。若提供signed/HDR扩展，另命名、另定义幂函数；当前8项没有该操作。输入曲线控制点x必须严格递增，禁止重复x含糊处理。

Monochrome是本清单易混淆的一项。线性RGB的Y来自当前原色矩阵；Lab L*是对Y/Yn的感知编码，提取后若要显示中性灰，应先按Lab逆变换或明确的L*→Y映射重建。可调channel mixer属于艺术黑白，不保证物理luminance。统一使用“黑白”名称却让下游猜输出尺度会影响阈值和示波器。

Hue-vs-X统一为`feature_curve`但保留明确操作模式：Hue-vs-Hue输出角度增量；Hue-vs-Sat/Chroma输出乘数或增量，二者不可混用；Hue-vs-Luma输出EV/加性L*等指定量；Luma-vs-Sat、Sat-vs-Sat、Sat-vs-Luma使用非周期source。建议首版周期piecewise-linear曲线并首尾连接，避免无意样条overshoot；hue在achromatic点采用固定不变策略。

HDR高光暗部需区分global zone和local operator。全局只依赖每像素feature；局部先分离低频base和detail，需要radius/guide/epsilon以及可能全局统计，不能标成E。Texture/Clarity/Dehaze为感知效果或成像模型的复合族：Texture可用band-pass增强、Clarity可用边缘保持base对比，Dehaze可用airlight/transmission模型；这些是实现候选，不是Adobe算法结论。

## 支持和实施

第一步支持线性Float32 RGB/RGBA与独立mask，渐次开放signed wide-gamut；Lab/OKLab操作必须等G2。调色默认对unassociated颜色操作，alpha原样；纯linear gain可优化为premul直接乘RGB。mask效果的混合空间明确，不能在Lab/encodedRGB/linearRGB之间静默切换。

master curve、RGB per-channel curves、luma curve、L* curve应在workflow中可见。LUT资源通过显式输入绑定，N或profile改变影响shape/契约时按静态路径处理；只改表值应以已允许的动态binding为目标，不假定当前端口都支持。

验收以灰阶ramp、色相环、饱和primary、肤色范围色块、negative/HDR、透明边缘组合；identity与neutral保持是基础。gamut mapping前后分别测量色差和目标分量，避免把压缩引入的变化误诊为主调整错误。灰度直方图与vectorscope用于使用检查，不能代替数值oracle。

最小使用链：`source transform→EV→WB→L*curve→Hue-vs-Chroma→mask→over→view transform`。每一级可作为独立节点编辑；显示结果始终记录使用的view和目标峰值亮度。

## 来源

[^clf]: Academy of Motion Picture Arts and Sciences，[*Common LUT Format Specification*](https://docs.acescentral.com/clf/specification/)，滚动官方规范；LUT与处理链语义。
[^levels]: Adobe，[*Levels adjustment*](https://helpx.adobe.com/photoshop/using/levels-adjustment.html)，滚动Photoshop指南；输入/输出色阶和中间调控制。
[^shadow]: Adobe，[*Adjust shadow and highlight detail*](https://helpx.adobe.com/photoshop/using/adjust-shadow-highlight-detail.html)，滚动Photoshop指南；局部radius与tonal width。
[^resolve]: Blackmagic Design，[*DaVinci Resolve Color*](https://www.blackmagicdesign.com/products/davinciresolve/color)，2026-09-09访问页面标示Resolve21；调色工具功能覆盖，未提供完整算法公式。
