# 工作流与可检查预期结果

下面 W1..W11 是新功能的**概念 DAG 与验收需求**，不是当前可直接执行的 operation key 列表。本次为文档研究，未运行这些流程。新功能实现时须使用公开 WorkflowDocument/compile/execute 入口补最小示例，不以私有 callback 单测代替使用验收。

## W0 当前已存在的使用基础

现有 [image vertical](../../../examples/image_vertical/main.cpp) 为 exposure→opacity；[regional image vertical](../../../examples/regional_image_vertical/main.cpp) 为 Gaussian→exposure→mask→source-over；[S4 example](../../../examples/s4_gpu_workflow/main.cpp) 为公开CPU/Metal调用与8项场景。命令、数据及既有oracle参见[现有图像文档](../../kernel-architecture/Image-Operations.md)。本次未重跑，不新增通过声明。

## 新需求流程

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
