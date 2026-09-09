# 软件功能覆盖与需求映射

本页按功能族整理七款产品，目标是发现可复用计算与支持缺口。证据为英文官方指南、发行页面及研究论文，访问2026-09-09。版本入口与功能页面日期分开；某功能页较旧不能证明当前版本行为完全不变。本次未逐菜单穷举、运行商业软件黑盒对照或计算无依据的覆盖百分比。

## 产品版本与代表性使用面

| 产品 / 版本证据 | 官方确认的相关能力 | 对本算子体系的映射 | 支持边界 |
| --- | --- | --- | --- |
| Photoshop27.10，2026-08 | 图层blend、选区与mask、调整、滤镜、Liquify、Content-Aware Fill、Auto-Align/Auto-Blend | format/grade/mask/filter/map/composite/retouch、多图配准与景深合成 | 模式位深限制、fill/opacity、ICC设置、生成工具与传统算法分别核验[^ps] |
| Lightroom Classic15.5 / desktop9.5，2026-08 | 照片显影、范围/语义mask、HDR与全景、Texture/Clarity、AI增强/移除 | RAW→scene→grade→mask→view；multi-image和模型 | Classic与desktop分开，不将目录/同步/相册当op；具体格式与模型支持有条件[^lr] |
| Camera Raw18.6，页面2026-08-27 | RAW与DNG、WB/显影、Denoise/Raw Details/Super Resolution、HDR/gain map | RAW子阶段、图像增强、profile与view、models | RAW入口与Camera Raw Filter支持面不同；Process Version影响外观[^acr] |
| DaVinci Resolve21，2026 | Color、Photo RAW、qualifier/windows/tracker、curves/Color Warper/HDR、scopes、空间/时域降噪、Magic Mask/Depth/Relight | grade/color-domain warp、scopes、mask、temporal、ML、摄影 | Free/Studio与功能条件需实现时核验；Photo页存在不代表与ACR算法相同[^resolve] |
| Clip Studio Paint5.0在线手册 | 图层/工具blend、vector、close-gap参考层填充、line width、dust、brightness→alpha、网点与Smart Tools | path/stroke、mask/fill、composite、插画清理、模型/复合效果 | 手册含edition/图层类型条件；“Smart”名称不足以证明神经网络实现[^csp] |
| Nuke17.1v1，2026-08-20 | 多通道/AOV、key/clean plate、STMap/SplineWarp、ZDefocus、Deep/Cryptomatte、时域、CopyCat/BigCat/Inference | data channel、map、PSF、matting、deep、time、models | 旧Merge公式只引用其固定版本；17.x移除legacy OFX不能推成所有同名新实现消失[^nuke] |
| After Effects26.3，页面2026-06-17 | channel/key/spill、blur/generate、blend/alpha工具、Roto Brush、tracking、time remap/displacement、Lumetri、OCIO | generation、mask/key、sampler、temporal、analysis、color管理 | 属性动画/时间线是宿主；每效果支持位深和时间需求独立[^ae] |

## 功能到基础、组合、宿主或模型

表中软件名为已找到的代表性锚点，不意味着未列产品没有此功能。实现方式列为本研究建议，不表示其私有内部实现。

| 功能族 | 代表性软件锚点 | 建议交付形式 | 规格入口 |
| --- | --- | --- | --- |
| 像素算术/通道配线 | Nuke channel/math、AE channel | 基础op | [numeric](../01-numeric/core.md)、[format](../02-format-color/representation.md) |
| RGB/感知曲线、1D/3D LUT | PS/ACR曲线、Resolve曲线/CLF/OCIO生态 | curve生成+apply+复合控件 | [curves](../01-numeric/curves.md)、[grade](../07-grade/adjustments.md) |
| ICC/打印预览/显示变换 | PS ICC、AE OCIO、Resolve Color | profile adapter+纯变换+宿主view | [format](../02-format-color/representation.md) |
| 程序图形/噪声/渐变 | AE Generate、CSP图形、Nuke Ramp/Noise类 | coord/shape/noise+ramp | [generation](../03-generation/generators.md) |
| 矢量与变化线宽 | CSP vector、AE shape/mask | PathSet+sample+coverage，UI编辑宿主 | [paths](../03-generation/paths.md) |
| 选区/填充/范围mask | PS选区、CSP close-gap、LR范围mask | mask/flood/EDT+selector workflow | [mask](../04-mask-morphology/masks.md) |
| 基础模糊/导数/锐化 | PS Filter、ACR detail、CSP Filter | kernel/statistics/signed response | [spatial](../05-filter/spatial.md) |
| Texture/Clarity/频率分离 | LR/ACR、PS工具组合 | multiscale decomposition/remap/reconstruct | [filter](../05-filter/frequency-restoration.md) |
| 物理/艺术景深与glow | Nuke ZDefocus、AE Lens Blur、CSP Lens Blur | depth decode/PSF/convolve/visibility组合 | [optics](../06-optics/bokeh.md) |
| Hue-vs-X与Color Warper | Resolve Color | 1D feature curve / 2D color grid，独立 | [grade](../07-grade/adjustments.md) |
| resize/affine/透视/镜头 | PS/ACR Transform、Nuke Transform/STMap | map generator+sampler | [geometry](../08-transform/geometry.md) |
| 液化/mesh/spline warp | PS Liquify、Nuke SplineWarp | stroke/grid→map→compose→single sample | [geometry](../08-transform/geometry.md) |
| 360/鱼眼 | 相机模型、Nuke投影/外部shader参考 | projection map；3D raster单列 | [projections](../08-transform/projections.md) |
| blend/holdout/group | PS/CSP/AE/Nuke | blend f+Porter-Duff+图层组DAG | [blending](../09-composite/blending.md) |
| key/matte/spill | AE Keying、Nuke IBK | key→refine→foreground→despill→over | [keying](../09-composite/keying-paint.md) |
| clone/heal/content-aware | PS Fill、LR Remove、Resolve patch | snapshot sampling+match/solve或独立模型 | [retouch](../09-composite/keying-paint.md) |
| scope/直方图/测量 | Resolve、Nuke、AE、PS histogram | raw statistics→scope render | [analysis](../10-analysis/scopes.md) |
| RAW显影/相机校准 | ACR/LR、Resolve Photo | decode+sensor+camera color+creative recipe | [RAW](../11-raw-photo/pipeline.md) |
| HDR/全景/景深堆栈 | LR HDR/Panorama、PS Auto-Blend | align→weights/select→reconstruct | [photo](../11-raw-photo/pipeline.md) |
| 时域、跟踪/重定时 | AE/Nuke/Resolve | frame requirements+flow+warp+temporal reduce | [temporal](../12-temporal-ml/requirements.md) |
| Deep/AOV/ID | Nuke/OpenEXR/Cryptomatte | 语义数据与专用merge/extract | [auxiliary](../12-temporal-ml/requirements.md) |
| 分割/深度/增强/生成 | 各产品模型工具 | 固定model与显式pre/post，独立训练 | [ML](../12-temporal-ml/requirements.md) |

## 当前覆盖差距的判断

现有8项足以证明一条RGBA曝光、蒙版、模糊、合成、预览与盖印路径；它们只覆盖本目录的较小基础子集。最先缺少的是跨通道/通用数据、辅助表、shape推断、任意坐标读取和统计输出组合。完成这些可解锁较多菜单功能，详见[路线](../14-roadmap/implementation.md)。

“数学等价”“视觉相近”“操作参数相近”“逐像素一致”分开验收。菜单名称和宣传例图最多支持功能存在，不证明默认空间、核、clipping、alpha或线程顺序。兼容需要指定版本、输入profile/位深、设置、导出数据与容差。

## 有意未完整展开的范围

文字排版/字体和文字动画、自然介质完整笔刷、漫画分页/UI、3D建模/材质/粒子/体积/USD/Gaussian splats、完整camera solve/立体校正、音频/Fairlight、剪辑时间线和资产管理、相机tethering、完整印刷RIP、每种私有RAW压缩、全部模型权重及商业私有公式。本目录为相关图像输入/输出和依赖留入口，不把这些外部产品系统计作首期算子验收。

## 来源

[^ps]: Adobe，[*Photoshop What's New*](https://helpx.adobe.com/photoshop/desktop/whats-new/whats-new-in-adobe-photoshop-on-desktop.html)，2026-08-28；[Content-Aware Fill settings](https://helpx.adobe.com/photoshop/desktop/repair-retouch/remove-objects-fill-space/adjust-content-aware-fill-settings.html)，2026-02-23。
[^lr]: Adobe，[*Lightroom Classic What's New*](https://helpx.adobe.com/lightroom-classic/desktop/introduction-to-lightroom-classic/whats-new.html)、[*Lightroom desktop What's New*](https://helpx.adobe.com/lightroom/desktop/introduction/whats-new.html)，2026-08；[Masking](https://helpx.adobe.com/lightroom-classic/desktop/process-and-develop-photos/masking.html)。
[^acr]: Adobe，[*Camera Raw Release Notes*](https://helpx.adobe.com/camera-raw/desktop/whats-new/release-notes.html)，2026-08-27；[Enhance](https://helpx.adobe.com/camera-raw/desktop/edit-and-enhance-images/sharpening-and-noise/enhance.html)，2024-10-14。
[^resolve]: Blackmagic Design，[*Photo*](https://www.blackmagicdesign.com/products/davinciresolve/photo)、[*Color*](https://www.blackmagicdesign.com/products/davinciresolve/color)，页面Resolve21；[21 New Features Guide](https://documents.blackmagicdesign.com/SupportNotes/DaVinci_Resolve_21_New_Features_Guide.pdf)，2026-04，具体功能与正式发行状态分开。
[^csp]: CELSYS，[*User Manual*](https://help.clip-studio.com/en-us/)，在线版5.0；[Advanced Fill](https://help.clip-studio.com/en-us/manual_en/420_fill/Advanced_Fill.htm)、[Smart Tools](https://help.clip-studio.com/en-us/manual_en/390_filters/Smart_Tools.htm)。
[^nuke]: Foundry，[*Nuke17.1v1 Release Notes*](https://learn.foundry.com/nuke/content/release_notes/17.1/nuke_17.1v1_releasenotes.html)，2026-08-20；[17.0v3](https://learn.foundry.com/nuke/content/release_notes/17.0/nuke_17.0v3_releasenotes.html)，2026-06-09，legacy OFX移除。
[^ae]: Adobe，[*After Effects Release Notes*](https://helpx.adobe.com/after-effects/desktop/what-s-new/release-notes-after-effects.html)，2026-06-17；[Keying](https://helpx.adobe.com/after-effects/desktop/animate-in-after-effects/keying/keying.html)、[Time Effects](https://helpx.adobe.com/after-effects/desktop/apply-effects-and-animation-presets/list-of-effects/time-effects.html)。
