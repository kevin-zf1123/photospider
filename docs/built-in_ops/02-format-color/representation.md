# 通道、数值格式与颜色管理

状态 Proposed。基础通道/数值/矩阵转换为D1；ICC/OCIO/profile、通用图像描述为D2。CPU Float32/Float64为参考，整数主要用于输入输出编码。逐像素数学多为E，但跨通道shape、完整profile/LUT辅助输入与当前端口的限制见G1..G5。

## 算子目录

| ID / 提议操作 | 输入 → 输出 | 关键参数和建议默认 | 实现/支持与验收 |
| --- | --- | --- | --- |
| FMT-01 `channel.extract` | `[H,W,C]`→`[H,W]` | `channel` 必填，0-based或精确通道名；拒绝重复歧义 | copy/view；提取alpha保留coverage角色，提取L*保留其单位；RGB不变相转灰 |
| FMT-02 `channel.merge` / `append` | 多个HW/HWC→HWC | channel order必填，默认严格同尺寸；禁止隐式resize | 颜色通道和AOV可拼接，输出必须验证新描述；extract→merge identity |
| FMT-03 `channel.swizzle` / `replace` | HWC+映射/源通道→HWC | indices/constant0/1；是否保留alpha显式 | RGB↔BGR、添加opaque alpha；G3通道数推断 |
| FMT-04 `alpha.associate` / `unassociate` | straight/premul→另一形式 | alpha角色必填；zero-alpha策略按profile | `Cp=a*C`，a>0时`C=Cp/a`；测试a=0/极小值/HDR，不对其他AOV相乘 |
| FMT-05 `alpha.set` / `extract` / `remove` | image+mask或颜色背景→image/mask | set是否重关联必须指定；remove选择丢弃或先flatten | 去alpha并不等于合成背景；opaque输入alpha=1 |
| FMT-06 `numeric.cast` | array→目标dtype | `round=ties_even`，overflow默认reject；clamp选项显式 | signed int8[-128,127]与uint8[0,255]区分；UInt16等需G1 |
| FMT-07 `numeric.encode_range` | float/integers→目标编码 | source/dest interval必填；dither默认off；round按标准覆盖 | 0..1→0..255线性缩放与cast分开；hdr不应静默clip |
| FMT-08 `color.assign` | image+descriptor→样本不变 | descriptor必填，不提供“猜颜色空间”默认 | 标签检查，不应称为颜色转换；特定位模式保持样本 |
| FMT-09 `color.transfer` | RGB/gray→同形 | transfer、direction必填；参数含PQ/HLG的单位与view环境 | 精确分段sRGB、gamma、log、PQ/HLG；alpha不编码 |
| FMT-10 `color.rgb_matrix` | linear RGB→linear RGB/XYZ | src/dst primaries、white；chromatic adaptation显式 | 3×3矩阵；允许signed与HDR目标，Float64算系数；neutral白点和round-trip |
| FMT-11 `color.model_convert` | 描述明确的颜色→目标模型 | src/dst模型、white、单位必填 | XYZ/Lab/LCh/OKLab/OKLCh/HSL/HSV等分别实现；无profile不能定义任意CMYK |
| FMT-12 `color.icc_transform` | image+ICC资源→目标profile | intent=relative colorimetric建议，black-point compensation显式 | CMM处理matrix/TRC/CLUT；CPU先行，GPU只在支持与误差验证后；G6 |
| FMT-13 `color.ocio_transform` | image+config/context→image | src/dst或display/view/look必填 | processor适配，保留配置语义；OCIO不替代通用ICC印刷变换 |
| FMT-14 `color.gamut_map` | 超目标域颜色→目标域 | method必填，首版clip与固定L/h降C分开 | per-channel clip改变hue；感知压缩给算法版本与迭代容差 |
| FMT-15 `color.tone_map` / `view_transform` | scene HDR→display目标 | 白点/峰值nits、black、transfer必填；算法预设显式 | 降动态范围与原色转换不同；保留中间HDR，只在输出端按目标映射 |
| FMT-16 `color.chroma_resample` | YCbCr planes→planes/RGB | 4:4:4/4:2:2/4:2:0、siting、range、matrix必填 | 低通后抽样，重建核、odd尺寸明示；G3/G4/G6 |
| FMT-17 `image.layout_convert` | planar/interleaved/strided→目标布局 | layout必填，数值/颜色不变 | packing、tile、row stride由存储实现处理；不把文件格式混入像素颜色模型 |
| FMT-18 `color.softproof` / `gamut_check` | image+source/proof/display profiles→proof image/gamut mask | output intent与proofing intent分开，纸白/黑点模拟显式 | CPU CMM，D2/G3；警告mask独立，不改工作原图；固定三profile比较意图差异 |

OpenImageIO官方提供通道重排、premult/unpremult、色彩变换等独立操作，支持这种职责拆分；Photospider的端口与默认值是本规格建议。[^oiio] ICC与OCIO分别采用其公开语义和资源配置，不能只以“色域名字”替代完整输入。[^icc][^ocio]

## 模型与支持面的精确定义

| 名称 | 语义和必要信息 | 首期建议 |
| --- | --- | --- |
| RGB/RGBA | RGB原色、白点、transfer及alpha关联；RGBA的A不是颜色维度 | Float32 linear-sRGB为已有profile；其他空间新表示 |
| Gray | 线性Y、编码luma Y′、Lab L*、艺术混色各不同 | 提供命名明确的提取算子 |
| Black/White | 二值阈值、抖动或halftone后的结果 | Float mask / UInt8编码，打包1-bit为输出codec职责 |
| XYZ / xyY | XYZ白点/归一化；xyY在XYZ总和或y=0时的策略 | signed Float32/64，黑点显式处理 |
| CIELAB / LCh(ab) | 参考白点；L*=0..100惯例，a*/b*可负；C与h为极坐标 | D50建议用于ICC连接，其他白点必须声明 |
| OKLab / OKLCh | 使用其规定的转换；OKLab L通常0..1尺度 | 按明确版本矩阵，signed cbrt，不能用Lab尺度套用 |
| HSL / HSV(HSB) | 对哪一种RGB数值计算；hue无色时无定义，L不是物理亮度 | 明确encoded RGB模式；achromatic策略固定 |
| YCbCr | RGB′的系数矩阵、量化range、chroma siting/sampling | BT.601/709/2020等各用命名模式；YCbCr与YUV不任意互称 |
| CMYK | 印刷条件、纸白、黑生成与总墨量通过profile决定 | ICC-only首版；没有profile就拒绝“真实颜色转换” |
| ACES | scene-referred体系；AP0/AP1与线性/log编码应分别标注 | OCIO配置或权威变换；固定版本与输入变换 |

颜色空间参考采用CSS Color 4对Lab/OKLab/极坐标和颜色转换的公开描述；CSS使用语境不自动规定Photospider的HDR工作空间。[^css] BT.2100的PQ/HLG、量化与显示参数必须使用指定版本的约定，其标准量化rounding不能被通用cast的ties-even默认覆盖。[^itu]

## 数学核心

`a≤0.04045` 时sRGB解码为 `a/12.92`，其他非负值为 `((a+0.055)/1.055)^2.4`。负值扩展另设策略。RGB原色转换使用线性值，经XYZ及所选白点适应，再到目标线性RGB。对编码RGB直接乘原色矩阵会得到不同结果。

Lab转换中 `f(t)=cbrt(t)` 当 `t>(6/29)^3`，否则 `t/(3(6/29)^2)+4/29`；`L*=116f(Y/Yn)-16`、`a*=500(f(X/Xn)-f(Y/Yn))`、`b*=200(f(Y/Yn)-f(Z/Zn))`。LCh为 `C=hypot(a,b)`、`h=wrap360(atan2(b,a)*180/pi)`，无彩时按显式策略。采样、白点与超范围策略影响结果，不能仅测试常见RGB颜色块。

## alpha 与 VFX 数据的边界

现有profile要求alpha=0时RGB=0。OpenEXR明确允许alpha=0而颜色非零的发光像素，普通VFX premultiplied输入因此不一定可直接绑定到现有端口。新设计应区分 coverage premul、可保留隐含颜色的straight、独立emission 或适合VFX的扩展语义。把这类样本清零会丢失信息，不能标成无损导入。[^exr]

颜色转换需要处理非线性与alpha交互：通常 unassociate→color transform→associate；极小alpha会放大误差，需设完整的数值策略。若包含独立emission，不可沿用简单除alpha公式。空间重采样、颜色变换和发光合成的wrapper分别建立。

## 使用与验收

概念链路：`decode→assign known source→unassociate→transfer decode→primaries/profile transform→grade→gamut/tone map→encode→quantize→host export`。若decoder已经输出线性camera RGB或已应用WB，必须从相应阶段进入，避免重复处理。

解析检查：identity matrix/通道重排往返；黑白与参考白；直通alpha位模式；0,.04045,1等分段点；灰轴中性；合法signed/HDR；整数上下限和halfway rounding；4:2:0奇数尺寸的chroma位置。profile/OCIO以固定资源和已知色块检查数值或ΔE，精度目标在该转换的实现规格中定义。CPU/GPU对比不能掩盖CMM算法差别。

## 来源

[^oiio]: OpenImageIO，[*ImageBufAlgo: Image Processing*](https://openimageio.readthedocs.io/en/latest/imagebufalgo.html)，滚动官方文档，访问2026-09-09。仅用于API职责示例，不据此宣称稳定版号。
[^icc]: International Color Consortium，[*ICC.1:2022 Profile Specification*](https://www.color.org/specification/ICC.1-2022-05.pdf)，2022-05；profile、PCS与rendering intent。
[^ocio]: Academy Software Foundation，[*OpenColorIO Documentation*](https://opencolorio.readthedocs.io/en/latest/)，滚动文档；配置、processor与display/view体系。
[^css]: W3C，[*CSS Color Module Level 4*](https://www.w3.org/TR/css-color-4/)，滚动规范草案；Lab、OKLab、色彩转换与色域概念。
[^itu]: ITU-R，[*BT.2100-3: Image parameter values for high dynamic range television*](https://www.itu.int/rec/R-REC-BT.2100)，2025；指定版本的PQ/HLG与量化要求。
[^exr]: OpenEXR，[*Technical Introduction: Premultiplied vs. Un-Premultiplied Color Channels*](https://openexr.com/en/latest/TechnicalIntroduction.html#premultiplied-vs-un-premultiplied-color-channels)，滚动官方文档；零alpha发光颜色。

Softproof接口参考：Little CMS，[*2.18 API*](https://www.littlecms.com/LittleCMS2.18%20API.pdf)，文档版本2.18、日期未核验；三profile、proofing intent与gamut checking。
