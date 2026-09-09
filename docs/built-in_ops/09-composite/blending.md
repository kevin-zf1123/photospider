# 混合模式与图层合成

状态 Proposed；标准数学模式为D1，商业兼容和图层组行为为D2。输入为前景S、背景B、可选mask/opacity，尺寸/工作空间先显式对齐。计算分为颜色混合函数与coverage合成，默认输出线性premultiplied图像；选择编码域的艺术混合时必须另标空间。所有公式中S/B是unassociated颜色，a/b是有效前景/背景alpha。

## coverage 合成

`c_out = Fs·c_s + Fb·c_b`，`alpha_out = Fs·a + Fb·b`，其中c为premultiplied样本。mask和图层opacity先改变源的有效alpha与关联颜色。以下系数采用Porter-Duff公开模型；各模式不意味着同名软件的其他开关也兼容。[^pd][^w3c]

| ID / mode | Fs | Fb | 意义 |
| --- | --- | --- | --- |
| CMP-01 clear | 0 | 0 | 清空 |
| CMP-02 source / destination | 1 / 0 | 0 / 1 | 选择单一输入 |
| CMP-03 source-over / destination-over | 1 / (1-b) | (1-a) / 1 | 前景/背景在上；source-over已有 |
| CMP-04 source-in / destination-in | b / 0 | 0 / a | 以另一输入coverage裁切 |
| CMP-05 source-out / destination-out | (1-b) / 0 | 0 / (1-a) | 去掉另一输入coverage |
| CMP-06 source-atop / destination-atop | b / (1-b) | (1-a) / a | 保留目标/源coverage的atop |
| CMP-07 xor | 1-b | 1-a | 仅非重叠区域；不是数值xor |
| CMP-08 plus | 1 | 1 | 颜色和alpha都相加；若alpha>1，需要独立表示/饱和政策 |

标准source-over与颜色混合f组合时：

`c_out=(1-a)c_b+(1-b)c_s+a*b*f(B,S)`，`alpha_out=a+b-a*b`。

这一定义适用于普通coverage颜色，不自动涵盖alpha=0而RGB非零的emission数据。source-over在当前8项内已实现，其他表项为提议。

## 逐通道混合

以下B,S默认在[0,1]的声明混合域；amount=1、mask=1、opacity=1为建议构造默认。需要HDR的无界代数变体单独命名，禁止静默套用bounded分支。每行的f仅算颜色，alpha仍由选定合成方式处理。

| ID / mode | f(B,S) 或定义 | 相近功能区别 |
| --- | --- | --- |
| BLN-01 normal | S | 图层普通合成 |
| BLN-02 add / subtract / divide | B+S / B-S / B/S | 数值运算可signed/HDR；divide除零默认reject；不等于plus alpha规则 |
| BLN-03 multiply | B*S | S=1中性；线性域与编码域结果不同 |
| BLN-04 darken / lighten | min(B,S) / max(B,S) | 每通道比较；whole-color明暗选择另算指定luminance |
| BLN-05 screen | B+S-B*S | B,S∈[0,1]；S=0中性 |
| BLN-06 overlay | B≤.5时2BS，否则1-2(1-B)(1-S) | 分支由背景决定 |
| BLN-07 hard_light | overlay(S,B) | 分支由前景决定 |
| BLN-08 soft_light_w3c | 采用明确的W3C分段定义，见下 | 多种软件公式不同，不只叫softlight |
| BLN-09 color_dodge | B=0时0；否则S=1时1；否则min(1,B/(1-S)) | 端点优先次序属于规格 |
| BLN-10 color_burn | B=1时1；否则S=0时0；否则1-min(1,(1-B)/S) | 与linear burn不同 |
| BLN-11 linear_dodge / linear_burn | min(1,B+S) / max(0,B+S-1) | 这里显式bounded艺术版本；无界add另列 |
| BLN-12 linear_light | clamp(B+2S-1,0,1) | S=.5中性 |
| BLN-13 vivid_light | S≤.5时burn(B,2S)，否则dodge(B,2S-1) | 组合公式需继承端点规则 |
| BLN-14 pin_light | S≤.5时min(B,2S)，否则max(B,2S-1) | 原清单point light暂按pin light整理，若指其他效果需更名 |
| BLN-15 difference / exclusion | abs(B-S) / B+S-2BS | 绝对差与signed subtract不同 |
| BLN-16 hard_mix | threshold(vivid_light,.5)的独立提议版本 | (B=0,S=1)输出0；与B+S>=1版本不同，商业fill/opacity待核验，D2 |
| BLN-17 darker_color / lighter_color | 根据指定L*/Y选择整组B或S | 避免逐通道min/max产生新的颜色 |
| BLN-18 dissolve | 以a为概率选择源coverage的seeded随机模式 | seed=0、全局像素坐标；彩色合成与时间稳定性单列 |
| BLN-19 emission_add | `RGBout=RGBb+gain*E`，alpha按显式keep_background | 依赖支持emission的G2；透明背景可产生零alpha非零RGB，当前facet受限版本应报错 |
| BLN-20 premul_mix | `(1-t)*A+t*B`对全部premul分量，t∈[0,1] | 同一图像原版与调色版插值可保alpha；不是将调色副本source-over回原图 |

BLN-03..10、15的有界公式可对照W3C；扩展模式11..19为本规格明确选择的数学版本，未宣称Photoshop bit identity。Adobe官方模式说明用于功能名覆盖，不作为未公开数值算法证明。[^w3c][^adobe]

soft-light定义：当S≤.5时 `B-(1-2S)B(1-B)`；否则 `B+(2S-1)(D(B)-B)`。当B≤.25时 `D(B)=((16B-12)B+4)B`，否则 `sqrt(B)`。kernel使用Scalar Float64 oracle，GPU FP32按容差比对。

## 非可分离颜色模式

默认提供 `blend_lab` / `blend_lch` 的明确语义：转换到共同参考白点的CIELAB，以LCh表示色相和chroma。下表描述重组后的unassociated颜色，再转换回目标工作空间，按独立gamut policy处理。

| ID / mode | 结果(L,C,h) | 备注 |
| --- | --- | --- |
| NBL-01 hue | (L_B,C_B,h_S) | 源无彩时建议保留h_B |
| NBL-02 chroma | (L_B,C_S,h_B) | 命名chroma，不能把C*声称为通用饱和度 |
| NBL-03 color | (L_B,C_S,h_S) | 保持背景L* |
| NBL-04 lightness | (L_S,C_B,h_B) | 同一参考白/归一化下L*唯一决定XYZ Y；L*数值或增量不等于Y，也不等于编码luma |

建议 `white=D50`、achromatic阈值按Lab单位显式设置，例如`C<=1e-6`；gamut policy必须显式选择，首版提供preserve浮点和clip，恒定L/h压缩为独立D2方案。OKLCh变体可用同结构，但单独命名和尺度，不能在同一mode中偷偷替换模型。

legacy `nonseparable_rgb_w3c` 单列：其Lum权重为0.30/0.59/0.11，Sat为max-min，依靠SetLum、SetSat和ClipColor构造Hue/Saturation/Color/Luminosity。该公开公式能够解释与HSL滤镜不同的一类行为，但**不足以证明Photoshop或CSP先做YCbCr变换**。RGB′权重也不能代表任意工作空间中的物理亮度。商业软件profile切换后的具体失真机制仍需版本固定的黑盒证据。[^w3c]

## 商业模式、组与alpha的开放项

CSP官方列出Add(Glow)与Glow Dodge并描述视觉用途，未公开完整alpha结算公式；不能据此选择“忽略alpha”实现并命名为兼容。应保留`U`标记，今后对a/b=0,.25,.5,1、HDR、opacity与fill分别测量。[^csp]

Nuke 12.2的Merge公开文档给出negative/HDR特别分支、alpha masking开关及soft-light公式，说明同名模式跨产品并不充分定义数学行为。这个证据固定于12.2文档，不能当作Nuke当前版本实测。[^nuke]

图层组还需要isolation/pass-through、group opacity、clipping mask、knockout、holdout、blend-if等组合语义，列为D2。它们由宿主图层模型编译成明确DAG；不能把UI图层树隐式塞进单个blend函数。形状coverage、颜色alpha、图层opacity和填充不透明度是否独立必须由产品功能说明。

## 实现、使用与验收

所有基础blend数学为O(HWC)、E，CPU SIMD/Metal适用；新通道和颜色转换需要G2，辅助mask需求按现有或新端口规则处理。常量与极值测试覆盖0/.5/1，dodge/burn特别覆盖(0,1)/(1,0)，multiply和screen中性元素，overlay与hardlight交换关系。

alpha测试对a/b各0,.5,1做解析矩阵；normal over不透明蓝的半透明红结果为(.5,0,.5,1)，透明源不影响背景。Lab模式测试目标L*或C*保持，超色域另报mapping引入偏差。dissolve要求ROI/whole/分块逐像素随机结果一致，并用大样本检查期望coverage，而非每小块恰好等于a。

典型使用：线稿multiply、发光emission、以mask在原图和grade结果之间premul_mix、LCh仅换色相、差图分析。将同一半透明图像的调色副本source-over回原图会增加alpha，不能代替默认保alpha的局部调整。difference可用于误差可视化，但定量差值应在signed数值层先计算，避免显示域夹紧。

## 来源

[^pd]: Thomas Porter、Tom Duff，[*Compositing Digital Images*](https://keithp.com/~keithp/porterduff/p253-porter.pdf)，SIGGRAPH 1984，pp.253–259；原论文镜像，alpha合成模型。
[^w3c]: W3C，[*Compositing and Blending Level 1*](https://www.w3.org/TR/compositing-1/)，Candidate Recommendation Draft，2024-03-21；Porter-Duff及有界separable/nonseparable公式。
[^adobe]: Adobe，[*Blending modes*](https://helpx.adobe.com/photoshop/using/blending-modes.html)，滚动Photoshop用户指南；模式功能描述，未作为所有分支数值兼容证据。
[^csp]: CELSYS，[*Blending modes*](https://help.clip-studio.com/en-us/manual_en/180_layers/Blending_modes.htm)，CLIP STUDIO PAINT在线手册；Glow模式名称与效果。
[^nuke]: Foundry，[*Merge*](https://learn.foundry.com/nuke/12.2/content/reference_guide/merge_nodes/merge.html)，Nuke12.2参考；HDR/negative/alpha开关和模式差异。
