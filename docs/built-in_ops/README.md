# 内建图像算子需求与研究规格

本文是内建算子规格与主题页的中文导航。各类别主题页是算子入口目录，`op_specs/` 下的英文规格只定义数值与算法、输入输出和行为；实现位置、后端、资源与性能不在本目录记录，内核执行与资源规则见 [kernel 架构指南](../kernel-architecture/README.md)。package、OperationTraits、Result C ABI 和产物格式的版本见[版本契约](../development/Compiler-Version-Contract.md)。

本目录面向 Photospider 算子实现者、workflow 作者和使用这些能力的图像应用。目标是覆盖 Photoshop、Lightroom、Camera Raw、DaVinci Resolve、Clip Studio Paint、Nuke、After Effects 中主要的图像处理与分析能力，并拆解为可组合、可验证的计算接口。

规格状态（Proposed/Accepted）、成熟度（D1/D2/D3）与实现状态分别记录在每份规格的 frontmatter，主题页的逐成员入口列出注册 key 与实现状态。商业软件逐像素兼容须有独立证据。

默认采用明确的数学与颜色科学语义。商业软件兼容模式只有在版本、工作空间、位深、alpha 和结果证据充分时建立。首期集中于确定性的静态图像计算；时域、跟踪、机器学习、深度合成纳入依赖规划。

## 阅读入口

| 目录 / 文档 | 内容 |
| --- | --- |
| [文档组织与规格模板](00-foundation/spec-template.md) | 主题页与英文规格的职责、规格包含的内容与验收模板 |
| [编译阶段与产物 API](../kernel-architecture/Compiler-and-Execution.md#compiler-artifacts) | CFG/SSA、静态 Plan、`.psir`/`.psxp` 保存与加载及错误处理 |
| [独立算子 oracle](../../oracle/ops/README.md) | 数值、生成、蒙版和滤镜参考程序及复现入口 |
| [综合研究报告](00-foundation/research-report.md) | 主要结论、设计取舍、专项核验与实施建议 |
| [候选依赖](00-foundation/dependencies.md) | 第三方库的职责、版本、许可标识和适配边界 |
| [公共数据与执行约定](00-foundation/contracts.md) | 数组、图像、颜色、alpha、单位、Region、确定性、错误 |
| [数值基础](01-numeric/core.md) | 表达式、序列、数组算术、比较、布局、归约、排序、扫描与矩阵 |
| [曲线与 LUT 生成](01-numeric/curves.md) | 控制点、插值、采样、1D 与 3D LUT 的差别 |
| [格式与颜色管理](02-format-color/representation.md) | 通道、alpha、存储、模型、ICC/OCIO/HDR |
| [生成器](03-generation/generators.md) | 常量、坐标、渐变、噪声与测试图 |
| [路径](03-generation/paths.md) | Bézier、宽度曲线、采样与光栅化 |
| [蒙版与形态学](04-mask-morphology/masks.md) | 选区逻辑、距离场、扩缩、羽化、连通域 |
| [空间滤镜](05-filter/spatial.md) | 卷积、秩滤波、边缘、锐化、边缘保持 |
| [频域与修复](05-filter/frequency-restoration.md) | FFT、PSD 基础、反卷积、去噪、图像修复 |
| [散景与光学](06-optics/bokeh.md) | 固定/可变 PSF、CoC、深度、光谱、优化 |
| [调色](07-grade/adjustments.md) | 100 个 tile 成员：基础调色、四条亮度路径、局部细节、加权统计、调色板抖动与半色调 |
| [几何变换](08-transform/geometry.md) | 重采样、仿射、STMap、置换、液化 |
| [鱼眼与投影](08-transform/projections.md) | 投影模型、畸变校正及 3D 光栅化边界 |
| [混合与合成](09-composite/blending.md) | 线性／编码域覆盖合成、逐通道及原生模型颜色混合 |
| [抠像与 matte](09-composite/keying.md) | 色键、背景估计、matting、去溢色与边缘延色 |
| [绘制与修补研究](09-composite/keying-paint.md) | 笔刷、clone、heal 等独立研究入口 |
| [分析与示波器](10-analysis/scopes.md) | 直方图、波形、矢量、频谱、PSD、统计 |
| [RAW 与摄影流程](11-raw-photo/pipeline.md) | 传感器校正、去马赛克、HDR/堆栈/全景 |
| [时域、模型与特殊数据](12-temporal-ml/requirements.md) | 帧序列、光流、跟踪、推理、deep/AOV |
| [软件覆盖矩阵](13-coverage/software-matrix.md) | 七款产品的功能族映射与证据范围 |
| [待决范围](14-roadmap/implementation.md) | 仍需专用决策才能定义的算法范围 |
| [工作流验收](14-roadmap/workflows.md) | 可检查的端到端使用链路 |
| [来源与证据边界](15-references/sources.md) | 一手来源索引、私有材料与待验证问题 |

## 最重要的拆分

1. 数组维数由数据含义决定：独立 RGB 曲线为 `[N,3]`，三维颜色 LUT 为 `[Nr,Ng,Nb,3]`，二维矢量场为 `[H,W,2]`。路径先保存拓扑和控制点，再采样与光栅化。
2. 颜色模型、RGB 原色/白点、传递函数、场景/显示参照、存储类型、数值编码范围和 alpha 关联方式分别定义。一个“格式转换”菜单应组合多个基础算子。
3. 滤镜的单通道数学核心可以复用到多通道；颜色图像包装层负责线性化、alpha 和颜色空间语义。有符号导数与复数频谱需要通用数据表示。
4. 菜单功能可能是一个基础算子、一个复合 workflow、交互工具，或依赖模型/时域的数据系统。目录中的功能覆盖不能换算为相同数量的独立 C++ 算子。
5. 已注册 key 以各主题页的逐成员入口为准。

## 规格完整程度

`D1` 表示给出可实现的数学核心、主要参数与验收；`D2` 表示功能族与算法选择已界定，仍须补专用接口或数据表示；`D3` 表示覆盖与依赖规划。D1 也不等于当前 ABI 已能承载，前置条件见每篇和公共契约。

文档中的“建议默认值”属于研究设计。调用者须显式提交规格中的必填参数；仅声明为 optional 的参数使用节点规定的默认值。
