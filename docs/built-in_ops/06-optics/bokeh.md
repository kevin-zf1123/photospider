# 散景、可变 PSF 与波动光学

状态Proposed。固定PSF与薄透镜CoC数学为D1；可变PSF、高质量屏幕景深为D2；光谱/波动光学为D3依赖规划。这里的薄透镜离焦弥散圆与caustic焦散不同。原对话《优化逐像素卷积》作为需求背景，公式与算法限制在下文重新界定。

## 相邻项目的实际基础

本地 `../phisical_bokeh` 当前代码采用每图层单距离、圆/多边形固定核；规格将非均匀depth和像差列为后续。以下为代码阅读事实，本次未重跑其测试或测量性能。

| 观察 | 本地证据 | 接入影响 |
| --- | --- | --- |
| x/y px/mm取几何平均 | `src/physical_bokeh_python/physics.py:18-30` | 等面积圆近似，未输出椭圆Rx/Ry |
| 距离m→mm、CoC取绝对值 | `physics.py:40-69` | signed CoC与depth编码需新增 |
| 直径<.5px用delta，奇数核；圆/多边形supersampling归一 | `aperture.py:140-191` | 离散阈值是该项目约定，无波长/OPD |
| 输入straight RGB+alpha，内部预乘再反预乘 | `convolution.py:141-194` | 不能直接传Photospider premul RGB，否则重复乘alpha |
| 固定核FFT，出口非负RGB/alpha clamp | `convolution.py:123-194` | 可供非负模糊参考，不是通用signed卷积 |
| depth/猫眼/像差列后续 | `doc/physical_bokeh_solver_spec.md:957-989` | 不能称已有逐像素景深 |

供本机阅读：[物理参数](/Users/zhufeng/document/code/phisical_bokeh/src/physical_bokeh_python/physics.py:18)、[卷积](/Users/zhufeng/document/code/phisical_bokeh/src/physical_bokeh_python/convolution.py:141)、[相邻规格](/Users/zhufeng/document/code/phisical_bokeh/doc/physical_bokeh_solver_spec.md:957)。这些绝对链接是私有工作区材料，公共读者需要另行取得源码；正文已给足可理解的观察边界。

## 算子分解

| ID / 功能 | 输入 → 输出 | 参数与支持 | 验收 |
| --- | --- | --- | --- |
| OPT-01 depth decode | depth+encoding→physical depth/validity | camera axialZ/radial/inverseZ/NDC、unit、camera必填 | 近远点、无效值、depth方向 |
| OPT-02 CoC from depth | depth→signed CoC和Rx/Ry | f(mm)、N、Sf(mm)、sensor mm、canvas、max radius | 对焦0、near/far符号、非方形像素 |
| OPT-03 aperture PSF | aperture参数→K[Kh,Kw] | disc/polygon/custom、blades≥3或disc、roundness[0,1]、rotation、aspect、sampling | 非负、归一、对称、subpixel采样收敛 |
| OPT-04 fixed PSF convolve | premul image+K→image | border、same/full、energy/throughput、anchor | impulse核形、透明边缘、HDR、直接/FFT一致 |
| OPT-05 variable gather | image+radius/PSF field→image | 输出中心选核，validity normalization显式 | 恒定半径与固定核同边界条件比较 |
| OPT-06 variable scatter | image+source PSF field→radiance_accum与weight/coverage_accum数值场 | 每源核归一，crop能量政策；输出不保证合法Image | 完整域总能量；常量不必保持常量，coverage可能>1，须G2/G3和后续可见性合成 |
| OPT-07 depth of field | RGBD或layered data→image | near/far、visibility、hole fill、quality mode | 遮挡边界与多层参考；单层缺失信息明确 |
| OPT-08 bloom/glare | HDR+PSF/threshold→光晕层/image | threshold、soft knee、gain=0默认、alpha/emission政策 | 不自动增加coverage，点光源与阈值连续 |
| OPT-09 motion/radial blur | image+trajectory/flow→image | shutter、samples、trajectory空间、center/angle | 静止identity、匀速轨迹，时域遮挡后期 |
| OPT-10 wave PSF | pupil amplitude+OPD+lambda→PSF | wavelength/长度单位、Fraunhofer/Fresnel、采样间距 | 非负、energy、OPD=0、lambda尺度；D3 |
| OPT-11 spectral apply | spectrum image或明确RGB代理→image | wavelengths、weights、sensor/CMFs、重建假设 | 色度积分与采样收敛；RGB代理不能标真光谱 |

固定层的PSF可首期直接复用非负RGBA流程；depth、vector、PSFBank、spectral与多输出需要G2/G3/G4。Foundry ZDefocus公开区分depth math、blur map、disc/bladed/image kernel和分层，支持以上接口拆分。[^nuke]

## 薄透镜与像素单位

建议内部距离统一mm，f>0、N>0、S>f、Sf>f，尺寸有限且正。项目约定近景CoC负、远景正：

```text
c_signed(S) = f² (S-Sf) / (N S (Sf-f))
Rx = abs(c_signed) * Wpx / (2 Wsensor)
Ry = abs(c_signed) * Hpx / (2 Hsensor)
```

c是sensor上的直径，Rx/Ry才是像素半径。几何平均px/mm保持面积但不保持椭圆形状；光圈图旋转与非等比例像素转换的顺序也须明确。S=∞用稳定极限 `f²/(N(Sf-f))` 或背景标记，避免Inf/Inf。

此公式的S按薄透镜物距建模。导入轴向Z、欧氏距离或inverse depth先解释相机模型，不能把灰度0..1直接当米。单目depth预测若没有尺度标定，只能给relative-depth艺术模式。

## gather 与 scatter

输出位置平均：`G(x)=sum_q I(q) K(x-q;R(x)) / sum_q K(x-q;R(x))`。

源能量散布：`S(x)=sum_q I(q) K(x-q;R(q))`，并保证每源核在完整输出域的离散和为1。

二者在恒定核、相同边界/归一条件下可重合；可变半径通常不同。scatter后无条件除总权重会改变能量语义。crop原画布会使能量离开输出，这是边界政策而非计算错误。

每源alpha归一不保证输出alpha≤1：一个opaque源用delta，邻源用包含该位置的五点均匀核，前者位置会累计1.2。OPT-06因此发布数值累积场，再由分层/遮挡/coverage政策生成图像；不得无条件clamp alpha补齐契约。

### 优化方案与精确范围

| 方法 | 成本与内存 | 精确条件 / 取舍 |
| --- | --- | --- |
| naive gather/scatter | O(CΣR²)，直接邻域 | 小图oracle，完整规定离散核和边界 |
| 行prefix gather | O(PC+CΣRy)，prefix内存O(PC) | 每行恒强连续区间；像素中心圆/椭圆测试，可精确求离散和 |
| 周界差分scatter+scan | 同上，差分缓冲O(PC) | 每源区间两端增减后前缀；核内恒强，源面积归一 |
| 半径/PSF分桶 | M次固定卷积+混合，成本由每核算法决定 | 桶间插值一般近似，不能称精确中间圆盘 |
| 低秩/复数基 | O(PCQr)级，Q为分量数 | 固定核低秩或复数基近似，负瓣/相位/遮挡误差需测 |
| 降分辨率+near/far+tile | 固定管线实时近似 | half-res、CoC-aware mip、边界外推具有质量代价 |

行prefix以`P_y(u)=sum(v<u)I(v,y)`计算区间`P_y(r+1)-P_y(l)`。scatter对源q的每条扫描线写`delta(l)+=I(q)/Aq`、`delta(r+1)-=I(q)/Aq`，最后沿行scan。Kosloff等描述了恒强任意形PSF的扫描线边界与积分扩展，复杂度与周界相关。[^kosloff]

精确性仅针对选定离散模型。连续prefix插值可精确积分一维分段常量信号，不自动等于二维圆与像素方块的面积交叠。supersampling/解析coverage是其他核；任意径向权重、中心相关深度相容性也可能破坏统一prefix。GPU随机写与归约次序会改变误差/性能，私有tile、确定性规约或scatter-as-gather必须测量。

分桶公式必须保留方向：gather为`sum_j a_j(x)(Kj*I)(x)`；scatter为`sum_j Kj*(a_j I)(x)`且每源权重和1。按R²插值可匹配理想连续圆盘二阶矩，但核会出现分段径向平台，小离散半径更需按真实核矩计算。重复box/圆盘不是任意圆盘的精确替代。Circular Separable Convolution是具名复数基近似，质量随分量数变化。[^circular]

## 遮挡与光谱

单张RGB加单层depth没有隐藏背景信息。质量层级建议：固定透明分层各自blur后合成；单RGBD的near/far与hole filling近似；layered depth/color；完整几何/光场积分。AMD FidelityFX DoF1.1使用降分辨率、mip、tile和近远分层，并明确单depth对半透明/体积的限制，可用作实时近似参考。[^amd]

RGB彩边代理、给定lambda的单色PSF、真正光谱图像应分层支持。标量非相干Fraunhofer模型可写`Pλ=Aλ exp(i2π OPD/λ)`，`PSFλ∝|FFT(Pλ)|²`。A是电场振幅透过率，输入强度透过率T时应采用sqrt(T)；OPD、lambda、pupil spacing、detector spacing必须联动。POPPY提供这些公开模型与多波长权重语义。[^poppy]

理想圆孔第一暗环半径约1.22λN_eff，近摄须考虑有效f-number；几何CoC与Airy半径不能直接相加冒充通用光学解。[^airy] RGB/XYZ到光谱是多解，任何RGB→spectrum都要声明重建基/光源假设，三个代表波长不等于物理光谱。[^color]

AE Camera Lens Blur的Diffraction Fringe名称不证明实现了Fourier光学；其公开说明描述环形能量再分配。[^ae] 猫眼口径蚀、旋焦、像差与焦外色差需要随位置/波长变化的PSF，不由单个固定ellipse解决。

## 使用、验收与未决项

首条流程：`HDR透明点→aperture PSF→premul blur→checkerboard over`。第二条：已知near/far图层先各自模糊合成作参考，再压成单RGBD比较屏幕近似。光谱后期流程：圆孔+lambda→PSF→encircled energy/MTF。

验收包括focus CoC=0、恒定CoC/固定PSF、单点总能量、完整域/crop区别、非方形Rx/Ry、alpha边缘、近景扩张、bucket误差、PSF二阶矩/L1/负旁瓣、lambda采样收敛。精度参考用Float64；差分重建的HDR相消可能需要分段/高精度累加。公开prefix沿扫描线有scan依赖；有静态最大半径的滤波可在带halo的块内建立局部prefix，数学支持仍有限。无界数据相关PSF才需要支持域预处理或Whole；辅助半径场端口仍须G4核验。

未决项：single RGBD的默认hole strategy；波动光学采样/能量单位；PSFBank表示与资源上界；real-time近似质量档。不得将邻仓已有测试源码称为本次通过结果。

## 来源

[^nuke]: Foundry，[*ZDefocus*](https://learn.foundry.com/nuke/content/reference_guide/filter_nodes/zdefocus.html)，滚动参考指南。
[^kosloff]: Kosloff、Tao、Barsky，[*Depth of Field Postprocessing for Layered Scenes Using Constant-Time Rectangle Spreading*](https://graphics.berkeley.edu/papers/Kosloff-DOF-2009-05/Kosloff-DOF-2009-05.pdf)，GI2009，§5；已核验官方索引正文，完整PDF读取失败。[作者博士论文](https://escholarship.org/uc/item/0161q94f)，2010补充PSF结构相关方法。
[^circular]: Garcia，[*Circular Separable Convolution Depth of Field*](https://media.gdcvault.com/gdc2018/presentations/Garcia_Kleber_CircularDepthOf.pdf)，EA/Frostbite，GDC2018。
[^amd]: AMD GPUOpen，[*FidelityFX Depth of Field1.1*](https://gpuopen.com/manuals/fidelityfx_sdk/techniques/depth-of-field/)，访问2026-09-09。
[^poppy]: POPPY，[*Overview*](https://poppy-optics.readthedocs.io/en/latest/overview.html)、[*Extending POPPY*](https://poppy-optics.readthedocs.io/en/latest/extending.html)，滚动官方文档。
[^color]: PBRT4，[*Color*](https://pbr-book.org/4ed/Radiometry%2C_Spectra%2C_and_Color/Color)，2023；光谱到颜色的多对一映射。
[^ae]: Adobe，[*Blur and Sharpen Effects*](https://helpx.adobe.com/after-effects/desktop/apply-effects-and-animation-presets/list-of-effects/blur-sharpen-effects.html)，Camera Lens Blur段。
[^airy]: Ansys，[*OpticStudio Standard Spot Diagram*](https://ansyshelp.ansys.com/public/Views/Secured/Zemax/v25101/en/OpticStudio_User_Guide/OpticStudio_Help/topics/Standard_Spot_Diagram.html)，v25101；Airy半径与成像侧有效f-number。
