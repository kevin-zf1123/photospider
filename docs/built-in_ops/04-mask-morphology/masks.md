# 蒙版、选区与形态学

状态Proposed。基本mask代数/有限footprint/离散EDT为D1，连续轮廓offset与复杂区域重建为D2。mask为Float32 `[H,W]`，finite[0,1]；SDF为signed距离场，label为Int64且0表示背景。默认画布外0，扩大画布须显式pad。

首版 threshold、四连通 labels、独立 count/area/bbox 已通过公开入口实现，范围、运行与结果见[组件算子](../../kernel-architecture/zh/Component-Operations.zh.md)；下述其余目录继续 Proposed。

## 软mask逻辑

| algebra | AND | OR | XOR/对称差 | NOT |
| --- | --- | --- | --- | --- |
| binary | 逻辑与 | 逻辑或 | 逻辑异或 | 1-A |
| fuzzy（建议默认） | min(A,B) | max(A,B) | abs(A-B) | 1-A |
| independent_coverage | AB | A+B-AB | A+B-2AB | 1-A |

MASK-01 `boolean`必须声明algebra，MASK-02 `invert`相对画布执行。A=B=.5时fuzzy结果为.5/.5/0，independent结果为.25/.75/.5。独立coverage假设与alpha合成相容，但只知道两个平均coverage无法确定真实子像素重合面积；精确几何Boolean应先组合路径再光栅化。[^comp]

## 算子目录

| ID / 功能 | 输入 → 输出 | 参数、算法 | Region/验收 |
| --- | --- | --- | --- |
| MASK-03 threshold/range | scalar→mask | threshold=.5、>=；soft width≥0 | E；端点、超范围与NaN策略 |
| MASK-04 color range | color+selector→mask | space、target、distance、inner/outer radius | E/W统计；中性、hue接缝 |
| MASK-05 dilate/erode | binary/gray mask→mask | footprint circle/square/diamond，radius=1px建议；max/min | H；r=0 identity、核大于图、边界 |
| MASK-06 open/close | mask→mask | erode→dilate / 反序，同footprint | 两步支撑累积；细桥、小洞 |
| MASK-07 geometric offset | contour/SDF→mask或SDF | signed radius，L2默认、L∞square/L1diamond | 连续边界与离散中心集合分模式；非整数必测 |
| MASK-08 Gaussian feather | mask→mask | sigma=1px建议、truncate/radius显式 | H；DC/边界；与距离羽化不同 |
| MASK-09 distance feather | SDF→mask | inner/outer widths、curve、轮廓位置 | E；0等值线和过渡宽度 |
| MASK-10 EDT/nearest feature | binary→distance/nearest coords | metric、spacing、signed/truncated、背景标签 | W；小图穷举最近点oracle |
| MASK-11 flood/region grow | image+seed(s)→mask | 4连接默认，8可选；与种子或邻居比较明确 | W；低对比大域、对角接触 |
| MASK-12 label components | binary→labels+属性 | connectivity=4，稳定label顺序建议按首像素 | W；0背景、细桥、跨tile组件 |
| MASK-13 fill holes/remove small | mask→mask | area px²、threshold与等号、foreground/background connectivity | W；孔洞与外部连通区不能混淆 |
| MASK-14 border/top-hat/black-hat | mask→field/mask | difference of morphology具名、输出范围 | H；灰度残差有signed语义时另类型 |
| MASK-15 skeleton/reconstruct | mask+marker?→skeleton/mask | topology、停止条件、distance weighting | W/迭代；连通性、端点、去枝规则 |
| MASK-16 close-gap fill | line/reference layers+seed→region | gap max px、连接候选、容差；仅临时边界 | W；原线稿保持、开口大小与误填 |
| MASK-17 combine/apply | image+mask→image | affect_result与restrict_samples分开 | 前者mix原图/滤镜结果，后者改变统计分母 |

灰度dilation是邻域最大值，作用在软mask上无需先阈值化。矩形min/max可两遍滑窗做到与窗口宽度无关的一维O(N)，任意圆形footprint和任意dtype不能自动继承同样复杂度。[^gray][^max]

## 非整数扩缩与距离

离散模式按像素中心的L2距离对实数r阈值化，结果仍随离散距离阶梯变化。连续模式先取得明确的矢量轮廓或重建等值线，再offset并计算像素coverage；从软mask的.5等值线开始会丢弃原软权重，必须显式选择。

真正signed distance约定内部d<0。膨胀r≥0得到`d<=r`，腐蚀得到`d<=-r`。distance feather要求inner/outer≥0；和>0时用`1-smoothstep(-inner,outer,d)`，inner=outer>0时轮廓coverage=.5；非对称宽度会改变该值。两者均0时使用hard threshold `d<=0`，不调用smoothstep。验收双零和单侧零宽。非方形pixel spacing必须进入度量。

Felzenszwalb–Huttenlocher的规则网格平方EDT为O(HW)，精确性针对离散输入中心距离，并非任意亚像素曲线距离。GPU PBA是精确EDT候选；Jump Flooding为近似，错误等级独立。[^edt][^pba][^jfa]

## 全局行为与使用

flood、label、hole fill、skeleton、reconstruction存在跨tile依赖，不能申请一个固定小halo就声称与全图相同。截断距离场可以设计有限支持版本，但“超截断全部同值”的输出语义与全距离图不同。参考实现CPU优先，label tie/order必须在并行路径固定。

典型流程：`参考线稿→close-gap→flood→remove small→offset .5px→feather→apply color`；`chroma selector→guided refine→mask grade`；`shape/path→Boolean→SDF→边缘效果`。

验收覆盖空/满、单点、孔洞、对角连接、细桥、触边、r=.25/.5/1.25、4/8邻域和非正方形spacing。全空/全满的EDT若无feature应采用显式无效标记/截断值策略，不能输出未经声明的Inf进入finite类型。

## 来源

[^comp]: W3C，[*Compositing and Blending Level1*](https://www.w3.org/TR/compositing-1/#porterduffcompositingoperators)，2024CRD；coverage代数。
[^gray]: SciPy，[*grey_dilation*](https://docs.scipy.org/doc/scipy/reference/generated/scipy.ndimage.grey_dilation.html)，访问v1.18.0。
[^max]: SciPy，[*maximum_filter1d*](https://docs.scipy.org/doc/scipy/reference/generated/scipy.ndimage.maximum_filter1d.html)，访问v1.18.0。
[^edt]: Felzenszwalb、Huttenlocher，[*Distance Transforms of Sampled Functions*](https://cs.brown.edu/people/pfelzens/papers/dt-final.pdf)，2012-09-02。
[^pba]: Cao等，[*Parallel Banding Algorithm*](https://www.comp.nus.edu.sg/~tants/pba.html)，I3D2010，作者页含2019更新。
[^jfa]: Rong、Tan，[*Jump Flooding in GPU*](https://www.comp.nus.edu.sg/~tants/jfa.html)，I3D2006。
