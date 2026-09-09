# 抠像、绘制与修补

状态Proposed。阈值key、clone与简单despill为D1；matting、heal、patch搜索为D2；生成式处理见模型规划。输入图像、hole/edit mask、允许采样区域、参考层和原始快照分别提供。默认Float32、线性或算法声明的颜色域；alpha/coverage/confidence/labels不可混用。

## 算子目录

| ID / 功能 | 输入 → 输出 | 方法、参数与边界 | 验收 / 使用面 |
| --- | --- | --- | --- |
| KEY-01 luma/range key | 图像+signal→matte | low/high、softness≥0；source=linearY/encodedY′/L*显式；E | 全低/全高/阈值端点、反转范围；局部选区 |
| KEY-02 chroma key | image+key color→matte | D1参考为共同白点Lab的DeltaE76距离→smoothstep；其他空间/亮度权重为D2 | key色0、远色1；灰前景/绿背景用固定阈值fixture，同色前后景不可仅凭颜色区分 |
| KEY-03 difference key | image+clean plate→matte | 颜色对齐后差值阈值/softness；先显式alignment/exposure correction | 相同图全0，局部变化可定位 |
| KEY-04 clean plate estimate | image+known-background mask→plate+confidence | 空洞估计、时间中位数或patch法具名；W/T | 已知背景区域保持，缺乏证据区低confidence |
| KEY-05 trimap matting | RGB+trimap→alpha及可选foreground | foreground/background/unknown编码显式；closed-form/具名求解；W | 固定FG/BG约束不变、未知域求解残差、内存 |
| KEY-06 foreground estimate | image+alpha+background evidence→F | `I=aF+(1-a)B`，小alpha正则化与噪声；E/H/W | 重新合成误差；alpha准确不代表F已恢复 |
| KEY-07 matte refine | matte+可选guide→matte | erode/dilate、hole fill、guided edge refine各具名 | 不把所有refine用侵蚀替代；细发丝和透明物体 |
| KEY-08 despill | F+spill色+mask→F′ | 指定色向投影/超量抑制、amount=0默认、亮度保持选项 | alpha不变，非spill区保持；去绿不等于缩alpha |
| KEY-09 defringe / edge color extend | F+alpha→F′ | 边缘颜色扩散、nearest-valid或局部估计，radius必填 | 不透明内部保持，透明边缘重合成无指定背景污染 |
| PNT-01 stamp | shape/texture+position+color→image/mask | source-over/replace/erase显式；已有brush_circle仅硬圆 | 固定输入可重放；增加AA/纹理与当前op分开 |
| PNT-02 stroke replay | ordered samples+brush settings+snapshot→image | samples包含position/time/pressure/tilt，spacing、flow、opacity明确 | 一条stroke内部累积规则、事件分批不变、无隐式设备输入 |
| PNT-03 clone | immutable source+target+source map+stroke mask→image | inverse resample source→masked replace/over；R/E | 修改mask外逐值保持，禁止读正在写的目的图造成不确定反馈 |
| PNT-04 heal / patch | source patch+target+mask→image | local mean/variance匹配、gradient-domain融合分别具名 | 边缘连续、纹理保留；同源identity |
| PNT-05 local inpaint | image+hole mask→image | Telea/Navier–Stokes等具名、radius与边界；H/W | 细划痕、小孔、触边大洞；mask外保持 |
| PNT-06 poisson blend | source gradients+target+domain mask→image | Dirichlet边界、solver tolerance/iterations；W | 解残差和边界约束，断连域/空域行为 |
| PNT-07 patch fill | image+hole+allowed-source mask+seed→image/correspondence | PatchMatch候选搜索→投票→重建；patch size/iterations/scale具名；W | seed=0、排除采样区、无有效source时报错 |
| PNT-08 defect repair | image/sequence+defect mask或检测→image+change mask | dust/scratch/dead pixel；检测与修复分离 | 真实星点/细线/网点的误检率、未修改区 |
| PNT-09 red-eye | RGB+eye regions→image+mask | 眼部定位与颜色校正分离、amount=0默认 | 眼白/高光保持，非眼部不得默认推断 |
| PNT-10 smudge / paint feedback | previous stroke state+brush samples→new state/image | 搬运颜色、混色、湿度等有状态过程；D2 | 状态顺序、分段重放、颜色守恒政策；不伪装纯无状态滤镜 |
| PNT-11 brightness to alpha | opaque artwork→line RGB+alpha | 提取指定亮度、反转或曲线→coverage，再按声明颜色重建 | 白底黑线端点，彩线的颜色保留策略 |

本表多输出是逻辑结果需求，当前单输出Value需G3决定承载。CPU参考先行；key/简单despill适合GPU，稀疏求解、补丁搜索和顺序paint需要独立并行与确定性方案。算法半径不一定等于依赖halo，迭代与连通求解按W或明确协调。

## D1 参考模式

KEY-01的`range_select`要求low<high、softness w≥0。w=0时选择low<=t<=high；w>0时输出`h(low-w,low,t)*(1-h(high,high+w,t))`，其中h为clamped三次smoothstep。反选为1-mask；测试所有四个过渡端点。

KEY-02的`lab76_key`先将图像与key色转同白点Lab，d为三分量欧氏距离；要求0<=inner<outer，输出`h(inner,outer,d)`。目标色d=0变透明，outer外不透明；低chroma前景是否保留由具体d和阈值决定，不保证区分同色对象。

KEY-08的D1 `green_excess`定义在unassociated linear RGB：`e=max(0,G-max(R,B))`，`G′=G-k*M*e`，R/B与alpha保留，k∈[0,1]默认0。其他色向投影、亮度保持和复杂自动spill估计保留D2；这个公式是数学版，不称为AE/CSP兼容。M=0和k=0必须identity，纯绿在k=1时降至max(R,B)。

## 可复用组合

Foundry的IBK将clean plate生成和key计算拆为IBKColor/IBKGizmo，适合借鉴其显式中间数据组织；不能据此宣称私有算法相同。[^ibk] Adobe AE推荐key之后接Key Cleaner再接Advanced Spill Suppressor，印证matte与颜色去污染应独立。[^ae]

典型链路：`clean plate→difference/chroma key→matte refine→foreground estimate→despill→premultiply→source-over`。mask产生器也可用于调色选择，不必绑定到抠像工具。trimap matting本身欠定，缺少可靠背景/前景约束时必须报告限制，不能把模型预测alpha称为测量真值。

Photoshop Content-Aware Fill对采样区域、颜色适应和新图层输出有控制；在算子层对应显式allowed-source mask和目标快照。[^ps] PatchMatch提供高效patch对应搜索基础，Poisson提供梯度域融合；二者是可实现方法，不构成Photoshop兼容证据。[^patch][^poisson]

## 动画与插画支持

CSP的参考层填充、close-gap、灰尘清除、线宽、亮度转透明度和网点清理需纳入基本能力。[^csp] 参考图层集合与实际着色目标分别传入；小缝闭合应输出临时闭合边界/区域mask，避免永久改动线稿。网点移除要记录原网频/角度和恢复方法，局部平滑、频域notch与模型修复是不同质量等级。

完整自然介质笔刷、笔刷素材系统、撤销和输入设备捕获属于宿主交互/后续状态模型。其可执行部分按明确笔迹和快照输入描述，不能承诺已有brush_circle涵盖CSP绘画引擎。

## 来源

[^ibk]: Foundry，[*IBKColor*](https://learn.foundry.com/nuke/content/reference_guide/keyer_nodes/ibkcolor.html)，滚动参考指南；clean plate与key组织。
[^ae]: Adobe，[*Keying*](https://helpx.adobe.com/after-effects/desktop/animate-in-after-effects/keying/keying.html)，2024-10-24；key、cleaner与spill顺序。
[^ps]: Adobe，[*Adjust Content-Aware Fill settings*](https://helpx.adobe.com/photoshop/desktop/repair-retouch/remove-objects-fill-space/adjust-content-aware-fill-settings.html)，2026-02-23；采样区等功能。
[^patch]: Barnes等，[*PatchMatch: A Randomized Correspondence Algorithm for Structural Image Editing*](https://research.adobe.com/publication/patchmatch-a-randomized-correspondence-algorithm-for-structural-image-editing/)，Adobe Research，2009-08-02。
[^poisson]: Pérez等，[*Poisson Image Editing*](https://www.inf.ed.ac.uk/publications/report/1094.html)，2003；作者机构原始论文记录。
[^csp]: CELSYS，[*Advanced Fill*](https://help.clip-studio.com/en-us/manual_en/420_fill/Advanced_Fill.htm)、[*Other Layer Filters*](https://help.clip-studio.com/en-us/manual_en/390_filters/Other_Layer_Filters.htm)，CSP5.0在线手册；填充与插画处理。

CSP灰尘、线宽与网点相关覆盖补充来源：[Filters](https://help.clip-studio.com/en-us/manual_en/390_filters/Filters.htm)、[Smart Tools](https://help.clip-studio.com/en-us/manual_en/390_filters/Smart_Tools.htm)，5.0英文在线手册。
