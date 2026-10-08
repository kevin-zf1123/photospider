# 工作流与可检查预期结果

下表区分当前可运行的 workflow 与保留的图像算子源码。图像算子仍使用旧 Value 图像端口时，不能据此视为当前 public Result 执行路径。W1..W11 是完整需求 DAG，概念链中的部分节点已有实现，不等于整条链全部交付。

## W0 当前已存在的使用基础

[image vertical](../../../examples/image_vertical/README.md) 与 [regional image vertical](../../../examples/regional_image_vertical/README.md) 是当前基于 Result API 的内置图像 workflow 示例，分别覆盖小型 exposure/opacity 图和 Gaussian 组合图的区域执行。它们取代这两个目录此前的 Value workflow 示例入口；[Unified Result image workflow](../../../examples/unified_result_workflow/README.md) 仍展示 test/example-only minimal operation。
05-filter 新规格与 oracle 仍是 Proposed，不列为已运行的内核流程。

## 已交付的公开 workflow

| 入口 / 对应需求 | 实际链路与注册方式 | 可检查结果 / 剩余范围 |
| --- | --- | --- |
| [Foundations](../../../examples/foundations_workflow)；W1 子集 | 默认 registry 的 numeric 与 expression/LUT 场景 | LUT x=.25→.125；RGB/滤镜流程尚无当前可运行实现 |
| [G4](../../../examples/g4_workflow/README.md)；W5 数据依赖子集 | radius scatter/gather、动态 demand、dependency cache 与 scan/reduction | 稀疏读取与依赖变化的预期输出见示例；图像 STMap 旧源码不能通过当前 Result 图像路径执行 |
| [Image vertical](../../../examples/image_vertical/README.md) | 默认 registry；Result RGBA input → `image.exposure_gain` → `image.opacity` → named Result output | `{frame=0,layer=0,y=0,x=1}` 单像素 ROI；独立 binary-fraction oracle、精确 source support、scalar control support 与四个 raw benchmark samples。本地 `example_image_vertical` 与安装包 `installed_image_vertical` 测试均通过。 |
| [Regional image vertical](../../../examples/regional_image_vertical/README.md) | 默认 registry；Result Gaussian → exposure → mask → source-over graph | 独立二维 Gaussian oracle、whole/ROI bitwise 对照、四种 power-of-two tile layout、broadcast/reversed input、halo 与 `UINT64_MAX` 检查，以及 zero-stride `65536²` source 上的 `5×7` ROI payload-budget 边界。本地 `example_regional_image_vertical` 与安装包 `installed_regional_image_vertical` 测试均通过。 |
| [S3 image workflow](../../../examples/s3_image_workflow/README.md) | 默认 registry；Result 图像场景、brush Result patch、slider、preview 与 export 的应用协调器 | Brush FIFO 容量 8、slider 取最新值、每 tick 最多一次编辑与一次图像步骤；kernel plan 使用 4×4 physical tile，而 export 独立发出 20 个 4×4 Result 请求。`preview` 与 `cache` 使用 CPU；它不是旧 Value tile-stream 的重现。本地与安装包 tests 均通过。 |
| [S4 GPU workflow](../../../examples/s4_gpu_workflow/README.md) | 默认 registry；Result 图像节点在 CPU 或 native Metal 上运行，含 resident-chain 与八项算法场景 | Whole、tiled、ROI、cache、preview/export、fallback 场景分开验证；CPU oracle 和 native Metal dispatch 独立说明。Metal 测试只在要求 native 且设备可用时作为 native 证据。本地 12/12、安装包 10/10 tests 通过。 |
| [Multi-output](../../../examples/multi_output_workflow/README.md) | 默认 registry 的 `image.split_horizontal` 源码使用 Value 图像端口 | 当前编译器要求图像使用 Result；可运行的 Result 输出选择示例见 [Unified Result](../../../examples/unified_result_workflow/README.md) |
| [Statistics](../../../examples/statistics_workflow/README.md) | 显式 `make_statistics_operation`；variant UInt8 `{1}` Result → 按需 Int64/UInt8 tensor Results → histogram → parameters → grade → Float64 tensor Result sink | `x[i]=(3*i+1)%8`，target=2；Counter/有理数 oracle 检查频数/mean/每个 pixel；空或零 mean 的 grade 明确失败 |
| [FFT](../../../examples/fft_workflow/README.md)；W10 频域子集 | 显式 `make_fft_operation`；variant Result → 按需 Float64 HW/HW2/HK2 tensor Results → FFT + imported response → multiply → inverse → Result sink | response `exp(-2*pi*i*(u/H+v/W))` 使图像循环下移/右移各一像素；独立 direct DFT；Full/Half、奇偶维度和 imaginary residual；PSD/Wiener 未交付 |
| [Components](../../../examples/components_workflow/README.md) | 显式 `make_component_operation`；variant Result → 按需 UInt8 HW tensor Result → labels → area → filter → Float64 tensor Result sink | `101/111` 的 `[id,area,min]=[1,5,0]`；filter 为 `label!=0 && area>=minimum_area`；BFS oracle、空 K、跨页细桥 |
| [Representations](../../kernel-architecture/Structured-Representations.md) | 有版本 schema → paged producer → Result consumer | Haar `[1,3,5,7,9]` 重建、空/动态字段、brush 分批、迭代 `[3,4]`；表示/helper 子集，不代表通用图像效果节点 |
| [Atom outcomes](../../../examples/atom_outcomes_workflow/README.md) | 公开 `execute_atoms` 与 joint contract 2 | 独立坐标结果、upstream failure provenance、Measured/受限 CertifiedBound；普通 execute 仍 fail-fast |
| [PNT-05A source](../09-composite/inpaint-ns-implementation.md) | Native Navier-Stokes implementation and optional OpenCV adapter remain in source; both expose the legacy Value image contract | 当前生产代码尚未适配 Result image slots，算法 callback 不能作为 public Result 图像 workflow 执行；数学与资源模型见链接 |

Layer 的纯 C++ 值 helper 不是已交付的 workflow。当前没有 Layer operation 或可运行的 Layer workflow；六种 Layer Result schema 和 Layer facet 均被拒绝。纯 helper 说明见 [Layer Runtime](../../kernel-architecture/Layer-Runtime.md)，W8 的图像集成仍属于原 roadmap 中的 Proposed 概念范围。

各个当前可运行的 numeric/result 示例链接包含仓库内构建、运行或安装包 consumer 命令。已有配置目录可按相关目标运行，
以下是入口说明：

```sh
cmake --build <build-dir> --target photospider_statistics_workflow photospider_fft_workflow photospider_components_workflow -j 8
ctest --test-dir <build-dir> -R '^(example_statistics_workflow|example_fft_workflow|example_components_workflow)$' --output-on-failure
```

统计的 `--stage-admission` 可单独检查 stage admission：2048×2048/B65536 在源绑定前拒绝；
2×513/B513 空输入需要 8 次源请求加 1 次完成 poll，cap 9 成功、cap 8 失败。
全局流程必须显式配置 managed resources；有限预算可能失败。测试中的 managed peak、
数值容差和页大小只代表各自 fixture，不能推广为 RSS 或任意尺寸完成保证。

## 完整需求流程

| ID / 使用者任务 | 概念链路 | 可检查输出与重要边界 |
| --- | --- | --- |
| W1 制作可修改 LUT | expression `y=x²`，x=0,.5,1 → 1D LUT → apply scalar ramp | 表 `[0,.25,1]`；线性查表 x=.25 得 .125，不能误称精确函数值 .0625；验证LUT采样误差 |
| W2 独立调 RGB 曲线 | channel extract → identity/red curve → channel merge → alpha | identity往返数值一致；红通道翻倍时绿蓝alpha不变；透明像素策略按profile |
| W3 软选区局部调色 | shape coverage → feather → mask arithmetic → grade → premul_mix(original,graded,M) | M=0得到原图，M=1得到完整效果且alpha保持；fuzzy交集(.5,.5)=.5，与概率乘积.25区分 |
| W4 矢量路径与变化线宽 | cubic controls → adaptive sample/arc length → width curve → stroke coverage → colorize → over | 直线长度100px，constant width10px；中心覆盖为1、远处0，亚像素边缘稳定；分段接头无多次alpha加深 |
| W5 可组合液化 | identity grid → brush displacement → compose maps → inverse remap | zero field identity；(dx=1,dy=0)下输出读取源x+1，检查内容运动方向；累计编辑不重复采样原图 |
| W6 边缘与分析 | linear luminance → Gaussian → gradient → magnitude/angle → Canny；另输出histogram | 常量图内部导数0；线性ramp导数为标定斜率；长弱边由强边连接跨tile保留 |
| W7 摄影调色 | tagged input → transfer decode → exposure EV → WB/adaptation → tone/gamut map → display encode | EV+1线性.18→.36（tone前）；neutral grey在目标白点中保持中性；HDR高光映射明确 |
| W8 标准合成 | unassociated red a=.5 → premultiply → source-over opaque blue | 线性premul输出(.5,0,.5,1)；先合成再encode，避免以显示编码数值冒充线性结果 |
| W9 散景 | depth(mm)+lens(mm,f-number) → signed CoC(px) → normalized source PSF → near/far composite | 对焦深度CoC=0；单点PSF在完整输出域总能量守恒；变半径与固定核一致性只在限定条件成立 |
| W10 频谱与修复 | signed field → window → FFT → PSD/频谱；另 Wiener→iFFT | impulse全频幅度常量；常量图无窗口时仅DC；正弦峰落到已知频率；PSD积分与窗口校正的能量一致 |
| W11 RAW 与多曝光 | decode→sensor corrections→demosaic→camera RGB transform；stack align→HDR merge→view transform | synthetic CFA重建常量色块；曝光比例2恢复同一参考尺度的相对radiance；无效/饱和区confidence输出 |

## 分析图如何检查

原始统计与显示图分开验收。2×2标量图 `[0,.25;.5,1]`，四个区间 `[0,.25),[.25,.5),[.5,.75),[.75,1]` 的 histogram 为 `[1,1,1,1]`。最后一个bin包含上界1；若存在超范围样本，须报告under/overflow而非丢弃。

全红测试图在RGB parade显示R高、G/B低；vectorscope点坐标依选定YCbCr矩阵和full/limited编码变化，测试应直接计算矩阵oracle，不能以“落在R标记附近”作为唯一验收。查看器的显示变换选择不应悄悄改变底层统计。

## 性能使用场景

至少区分：小 ROI 交互、全尺寸静态导出、大半径局部滤镜、全局分析、含中间精度转换的混合流程。报告实际尺寸/半径/backend、质量参数、峰值受控字节和耗时；对比相同数学语义及容差，不能把更小分辨率近似当成同质量加速。

这组流程是需求验收的最小横切面，不要求每项修改重跑全部场景。某一步实现改变后，只运行直接受影响流程和必要的边界检查。
