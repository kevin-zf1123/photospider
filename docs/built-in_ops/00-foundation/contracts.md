# 公共数据与执行约定

2026-09-10：本轮已接受子集以英文 [ADR 0020](../../adr/0020-composable-operation-foundations.md) 为准。保留四种 dtype、采用 typed semantics/image v2、静态输出和 computed scalar；image v1 不保留。下面路径、时域、外部资产及更广泛颜色功能继续为 Proposed，不构成本轮实现范围。

以下为 **Proposed** 的统一设计规则。当前实现仍按[现状页](current-state.md)执行。新算子默认继承本页；分类页的显式参数和例外优先。这些描述是语义目标，不是新的 C API 声明。

## 数据对象

| 对象 | 逻辑形状 | 必须附带的解释 |
| --- | --- | --- |
| scalar / sampled signal | `[1]` / `[N,C]`，C=1 可约定 `[N]` | 自变量起点、间距/显式 positions、端点、单位 |
| image | `[H,W,C]` | 通道名及角色、颜色描述、alpha 角色、像素纵横比 |
| mask | `[H,W]`，Float32 [0,1] | coverage / probability / fuzzy membership 分开 |
| signed scalar field | `[H,W]`，Float32/64 | 距离、梯度、深度、频率等单位；不能假借 mask |
| vector field | `[H,W,2]` 或 `[H,W,3]` | 分量坐标系、像素/归一化单位、forward/inverse |
| complex field | `[H,W,2]` 的实/虚约定或未来 complex 类型 | FFT 原点、顺序、符号、归一化；不可作为颜色两通道 |
| independent-channel 1D LUT | `[N,C]` | 每通道同一自变量轴、domain、插值、越界 |
| coupled RGB 3D LUT | `[Nr,Ng,Nb,3]` | RGB 轴顺序、输入/输出空间、domain、插值 |
| path | controls + offsets + segment kinds + closed flags | 拓扑、方向、fill rule、坐标和 width profile |
| sequence / deep samples | 帧索引集合 / offsets+samples | timestamp/timebase、缺帧 / per-pixel sample depth 与 opacity |

多数组对象是概念表示，当前单输出 Value 的承载方式尚待定义。空集合优先采用明确的有效计数/元数据方案进行设计评审，不能构造当前不合法的零长度 shape。避免为路径武断指定“四维数组”；`[segment,control,xy]` 只适合固定次数且固定拓扑的子集。

## 颜色与 alpha

颜色描述拆成：模型、通道顺序、原色/白点或 ICC profile、传递函数、scene/display reference、绝对/相对亮度单位、alpha association。YCbCr 还需 matrix coefficients、full/limited range、chroma subsampling/siting。存储 dtype、量化位数与编码区间另行记录。

`assign_profile` 只重新解释标签；`convert_color` 才改变样本值。RGB 原色转换、transfer decode/encode、chromatic adaptation、ICC rendering intent、tone/gamut mapping 各有独立语义。不能把 sRGB 精确分段传递函数写成固定 gamma 2.2。

颜色滤镜一般先取得 unassociated 颜色，处理后重新关联 alpha；空间重采样和物理线性模糊通常处理线性 premultiplied RGB 与 coverage。非线性颜色空间不能直接对 premultiplied RGB 运行转换。alpha=0 的隐含颜色是否保留由表示明确约定，当前 RGBA profile 只允许零 RGB。

显示编码、负值与 HDR 不默认夹紧。每项操作明确 `reject / preserve / clip / map`；bounded mask 保持 [0,1]，而 derivative/FFT/曝光场可有负值。数值除零默认报错，只有定义了数学边界的算子采用其专用极限。

## 坐标与边界

图像数组顺序 y,x,c；几何向量顺序 x,y。建议像素覆盖 `[x,x+1)×[y,y+1)`、中心 `(x+0.5,y+0.5)`，左上为原点，y 向下；接入整数中心坐标的库时使用显式适配。角度对外建议 degrees，内部 radians；旋转正向必须描述在 y 向下坐标中的方向。每个变换注明输入/输出 pixel aspect 和 data/display window。

标准边界模式：constant、clamp、reflect、mirror、wrap，分别给采样序列定义。reflect 与 mirror 的重复边缘规则不能依赖库名称猜测。边界扩展作用于完整逻辑图像，不作用于 tile 的临时边缘。默认局部滤镜建议 clamp，几何域外建议 constant transparent；具体规格可覆盖。

## 执行类别

| 简记 | 数学依赖 | 当前落地约束 |
| --- | --- | --- |
| E | 同像素/同样本 | Elementwise 仍须符合端口 shape 与语义约束 |
| H(r) | 有限半径邻域 | 已有对称静态 Halo；单轴/各向异性可用保守对称边界 |
| W | 全局或动态无法有界局部推导 | Whole；全部输入与 scratch 必须能放入预算 |
| R | 输出坐标反推输入覆盖 | 仅当前 Shrink 有专门实现，其他先 Whole 或新契约 |
| S | prefix/scan/递推 | 跨 tile 状态依赖，先 Whole；不伪装 H |
| T | 需要多个时间点 | 后期序列契约，显式时间窗与边界帧 |

全局统计按“局部累积、确定性归约、应用统计”拆分；数学上可以流式并不意味着当前内核已有通用 reduction 协议。任意 remap 的保守输入范围要覆盖插值核及非线性极值，dirty 正向传播也需要对应规则。

LUT、kernel、控制点等小型辅助输入通常需要全表，而图像输入只需局部 ROI。E/H 是数学分类，不能据此直接设置现有 Elementwise/Halo trait：当前 generic 输入存在 shape 匹配限制。首版可明确使用Whole，后续以真实流程论证按输入端口的完整辅助表需求。

## 数值、随机与资源

建议首期 Float32 运算数据，Float64 用于权重、统计和参考计算；确有存储/外部消费需求再增加其他 dtype。逐操作规定 CPU exact 或 tolerance 等级，不能要求所有超越函数跨平台 bit identity。GPU FP32 不支持某数值范围或精度目标时按已定义语义回退；后端与质量模式进入适用缓存身份。

随机生成建议将 seed、全局坐标、通道、帧号、stream ID 映射到 counter-based 随机序列，以保证分块/调度改变不改变样本。正态变换等浮点步骤仍需定义 CPU/GPU 的误差规则。该设计借鉴 Random123 的无状态计数器思路，不声称随机生成天然跨所有后端逐位相等。[^random]

每个 op 给出输出与 scratch 上界、可取消循环粒度和缓存依赖。表达式 AST、LUT、profile、kernel、模型配置的实际语义影响结果；外部可变路径字符串不足以表示一次执行的不可变输入。由宿主装载并显式绑定，不让纯图像算子隐式读写文件。

## 公共验收

默认有限浮点参考建议先采用 `atol=1e-6, rtol=1e-5` 作为候选，算法作者必须按尺度/累积/质量目标修订。颜色变换另报指定空间的 ΔE；mask/path 可报 coverage 和轮廓误差；随机报分布与频谱；几何报亚像素位置误差。解析恒等式优先于单纯截图。每个新增确定性局部算法检查 whole/ROI/tile 一致性。

[^random]: John K. Salmon 等，*Parallel Random Numbers: As Easy as 1, 2, 3*，SC11，2011；[作者与项目页](https://random123.com/)。支持 counter-based 并行随机数设计，具体 Photospider 键布局和质量模式仍为建议。
