# 工作流与可检查预期结果

2026-09-13：下面分别列出已有公开可执行示例和 W1..W11 的完整需求 DAG。概念链中的部分节点已有实现，不等于整条链全部交付。示例和检查来自当前源码，本轮仅同步文档，未重新运行产品流程。

## W0 当前已存在的使用基础

现有 [image vertical](../../../examples/image_vertical/main.cpp) 为 exposure→opacity；[regional image vertical](../../../examples/regional_image_vertical/main.cpp) 为 Gaussian→exposure→mask→source-over；[S4 example](../../../examples/s4_gpu_workflow/main.cpp) 为公开CPU/Metal调用与8项场景。命令、数据及既有oracle参见[现有图像文档](../../kernel-architecture/Image-Operations.md)。本次未重跑，不新增通过声明。

## 已交付的公开 workflow

| 入口 / 对应需求 | 实际链路与注册方式 | 可检查结果 / 剩余范围 |
| --- | --- | --- |
| [Foundations](../../../examples/foundations_workflow)；W1/W2/W3 子集 | 默认 registry 的 expression/LUT、channel/alpha、basic-curves/masks/filters/fields | LUT x=.25→.125；曲线 workflow 保留 alpha=.5；非对称卷积/相关差 `[4,4,4]`；通用 shape/路径仍待实现 |
| [G4](../../../examples/g4_workflow/README.md)；W5 采样子集 | computed STMap → image；radius scatter → snapshot patch | 稀疏源像素 {0,1023} 共 32 bytes；远处 radius 修改触发依赖；完整 liquify/map compose 仍待实现 |
| [Multi-output](../../../examples/multi_output_workflow/README.md) | 默认 registry 的 420、split、per-channel convolution、Gaussian + kernel | 按端口独立 ROI；kernel-only 无图像样本读取；joint/singleton 结果等价 |
| [Statistics](../../../examples/statistics_workflow/README.md) | 显式 `make_statistics_operation`；Int64 source + UInt8 mask → histogram → parameters → grade → active sink | `x[i]=(3*i+1)%8`，target=2；Counter/有理数 oracle 检查频数/mean/每个 pixel；空或零 mean 的 grade 明确失败 |
| [FFT](../../../examples/fft_workflow/README.md)；W10 频域子集 | 显式 `make_fft_operation`；Float64 source → FFT + imported response → multiply → inverse → sink | response `exp(-2*pi*i*(u/H+v/W))` 使图像循环下移/右移各一像素；独立 direct DFT；Full/Half、奇偶维度和 imaginary residual；PSD/Wiener 未交付 |
| [Components](../../../examples/components_workflow/README.md) | 显式 `make_component_operation`；UInt8 mask → labels → associated area → filter | `101/111` 的 `[id,area,min]=[1,5,0]`；filter 为 `label!=0 && area>=minimum_area`；BFS oracle、空 K、跨页细桥 |
| [Layer](../../../examples/layer_workflow/README.md)；W8 扩展 | 显式 `make_layer_operation`；assemble/over/emission/flatten 与 contributions→weighted reduce→optional | RGBA `[12.5,-2,1.25,1]`；W=0 valid=false；midpoint tree、严格 association underflow、最终 window 释放 |
| [Representations](../../kernel-architecture/Structured-Representations.md) | 有版本 schema → paged producer → Result consumer | Haar `[1,3,5,7,9]` 重建、空/动态字段、brush 分批、迭代 `[3,4]`；表示/helper 子集，不代表通用图像效果节点 |
| [Atom outcomes](../../../examples/atom_outcomes_workflow/README.md) | 公开 `execute_atoms` 与 joint contract 2 | 独立坐标结果、upstream failure provenance、Measured/受限 CertifiedBound；普通 execute 仍 fail-fast |
| [PNT-05A](../09-composite/inpaint-ns-implementation.md)；当前 ops-specs 附加实现 | 默认 native key 或可选 OpenCV key；5×5 opaque RGBA + 中心 hole → `image` | radius=3，常量图重建 `[.25,.5,.75,1]`；Whole、输入限制与现有验收缺口见链接 |

各示例链接包含仓库内构建、运行或安装包 consumer 命令。已有配置目录可按相关目标运行，
以下只是入口说明，并非本轮运行记录：

```sh
cmake --build <build-dir> --target photospider_statistics_workflow photospider_fft_workflow photospider_components_workflow photospider_layer_workflow -j 8
ctest --test-dir <build-dir> -R '^(example_statistics_workflow|example_fft_workflow|example_components_workflow|photospider_layer_workflow)$' --output-on-failure
```

统计的 `--stage-admission` 可单独检查 #325：2048×2048/B65536 在源绑定前拒绝；
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
