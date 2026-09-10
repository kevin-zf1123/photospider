# 图形、坐标、渐变与噪声生成

2026-09-11：本轮基础子集的接口、Region 与可运行示例见[基础算子实现](../../kernel-architecture/zh/Basic-Operations.zh.md)。其他目录项继续保持原研究状态。

状态Proposed，基础为D1；mesh gradient、动态点集合与物理噪声为D2。生成器必须显式给输出shape/坐标描述；数学逐像素可计算不等于当前registry具有按参数推导尺寸的契约。默认像素中心 `(x+.5,y+.5)`，图像左上为(0,0)，数组y,x,c。

## 图形与坐标

| ID / 功能 | 输入 → 输出 | 参数与方法 | 验收 |
| --- | --- | --- | --- |
| GEN-01 constant | shape+value→image/field | 通道角色显式，constant=0；E | 任意ROI值一致，alpha不猜 |
| GEN-02 coordinate grid | shape→[H,W,2] | pixel/normalized edge坐标；identity source map用像素中心 | 相邻x差1px，宽W的归一坐标为(x+.5)/W |
| GEN-03 basic shape | rectangle/ellipse/polygon/star→mask或SDF | position/size、fill、AA误差；先mask再着色 | mask外0；SDF内负外正（或正截断值），面积/亚像素移动另测 |
| GEN-04 test patterns | shape+pattern→field/image | checker/grid/ramp/impulse/zone plate/Siemens star/color bars | 频率、中心、强度明示；采样测试可解析 |
| GEN-05 gradient coordinate | geometry→scalar t[H,W] | linear/radial/angular/diamond/box/path-distance | 几何与颜色表分离；退化参数显式报错 |
| GEN-06 gradient lookup | t+ramp[N,C]→image | pad/repeat/reflect；颜色空间和alpha解释必填 | 端点/接缝/透明彩色中点 |
| GEN-07 mesh/bilinear gradient | 网格/四角颜色→image | 拓扑、插值基函数、折叠和gamut；D2 | 控制点值、边界连续、非方形画布 |
| GEN-08 point distributions | bounds+seed+density→points[M,2] | grid/jitter/Poisson disk；min_distance>0 | 最近距离、seed、动态M上界；G3 |

Linear用`dot(p-a,b-a)/|b-a|²`；radial用`|p-c|/r`，椭圆先变换局部坐标；angular用`atan2`并映射到周期，中心与接缝显式；diamond/box采用L1/L∞等值线。two-circle radial需要根选择和无实根策略，保留D2。SVG明确包含坐标系、变换和spread method，只有颜色数组与尺寸不足以决定二维渐变。[^svg]

颜色插值建议默认linear RGB，显式可选Lab/OKLab/LCh；极坐标hue有shorter/longer/increasing/decreasing，无彩色端点和alpha不能只按四个普通数插值。CSS Color4可作命名语义来源；Photospider需固定所选子集及版本。[^css]

## 噪声

Gaussian表示分布，white/blue表示频谱，Perlin/Voronoi表示构造。将这些维度分开定义，既能产生Gaussian白噪声，也能产生具有指定相关长度的Gaussian场。

| ID / 功能 | 输入 → 输出 | 建议参数与算法 | 支持 / 验收 |
| --- | --- | --- | --- |
| NOI-01 white uniform | shape/seed→field | seed=0、range[0,1)、stream/channel显式 | counter RNG，E；均值.5、方差1/12和相关性 |
| NOI-02 white Gaussian | shape/seed→signed field | μ=0,σ=1、具名Box–Muller等；不clip | log(0)保护，signed G2；均值/方差、分布尾部 |
| NOI-03 correlated noise | white field+filter→field | correlation length px、边界与方差归一 | H/W；与white频谱不同 |
| NOI-04 Perlin | coordinates+seed→field | cell size必填，算法版本固定、gradient/fade/hash明确 | O(P)，E；格点连续性和周期，输出范围按版本 |
| NOI-05 cellular/Voronoi | coords+feature process→F1/F2/ID | L2建议，每cell点数/seed固定 | 最近点搜索；F2-F1不是精确边界距离 |
| NOI-06 fractal | base noise→field | octaves=4,lacunarity=2,gain=.5建议；fBm/turbulence/ridged分别具名 | O(P·octaves)；频率随采样密度带限 |
| NOI-07 blue noise | 固定tile/seed+coords→field/points | 周期、通道与资源版本明确 | PSD低频抑制、周期重复；生成点集不等于像素mask |
| NOI-08 Poisson/shot | calibrated signal+gain→noisy field | electron/count单位、exposure、read noise分开 | 方差随均值；不能仅对RGB加独立同方差噪声 |
| NOI-09 speckle/grain | signal+model→field/image | multiplicative speckle与艺术film grain独立 | noise power/相关尺度/颜色相关、曝光依赖；D2 |
| NOI-10 continuous temporal noise | x,y,t→field sequence | 连续维noise或时空blue，time单位显式 | T/D3；逐帧独立随机图不表示连续演化 |

Perlin2002的固定gradient/permutation与五次fade为明确可实现的版本，换hash或seed展开就改变图样，应纳入算法身份。[^perlin] Random123的counter-based方法适合按全局坐标取样；整数序列和正态浮点变换的跨后端一致性分别定义。[^random] 时空blue noise具有独立时间频谱设计，不能以每帧新2D蓝噪声替代。[^blue]

默认随机key由seed、stream、绝对坐标、channel及可选frame构成，不依赖tile顺序或局部ROI原点。cache依赖seed和全部生成参数。Gaussian与其他signed输出不能使用当前非负RGBA facet。

## 使用与验收

`noise→range→mask/color ramp/displacement`可复用到纹理、转场、选择与液化；AE Cell Pattern官方列出matte和displacement用途，可作为使用面锚点。[^ae] `shape→coverage→colorize→over`同时服务选区与绘制。普通图形首版无需额外实现颜色合成。

解析测试覆盖坐标identity、线性渐变0/1端点、非法半径、repeat/reflect、全局seed一致；随机统计使用固定样本数与置信阈值，不能要求每次精确达到理论均值。shape/path coverage的误差用面积/轮廓检查，不仅看图。

## 来源

[^svg]: W3C，[*SVG2 Paint Servers*](https://www.w3.org/TR/SVG2/pservers.html)，2018-10-04 CR；渐变几何与spread。
[^css]: W3C，[*CSS Color4 Interpolation*](https://www.w3.org/TR/css-color-4/#interpolation)，访问2026-09-09；颜色/极坐标/alpha插值。
[^perlin]: Ken Perlin，[*Improved Noise reference implementation*](https://mrl.cs.nyu.edu/~perlin/noise/)，2002。
[^random]: D. E. Shaw Research，[*Random123*](https://github.com/DEShawResearch/random123)，SC11论文2011及官方实现。
[^blue]: Wolfe等，[*Spatiotemporal Blue Noise Masks*](https://research.nvidia.com/publication/2022-07_spatiotemporal-blue-noise-masks)，NVIDIA Research，2022-07-06。
[^ae]: Adobe，[*Generate effects: Cell Pattern*](https://helpx.adobe.com/after-effects/desktop/apply-effects-and-animation-presets/list-of-effects/generate-effects.html)，滚动英文指南。
