# 工作流与可检查预期结果

下表列出现有 public workflow 与直接行为测试。需求 DAG 中的概念链只表示组合目标；其中的节点并不因此自动成为已注册功能。

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
