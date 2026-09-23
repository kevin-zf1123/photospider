# 复杂表示契约

英文权威文档：[Structured-Representations.md](../Structured-Representations.md)。

安装公共头 representation.hpp 提供八种 version-1 表示。*_schema 检查静态配置，
*_spec 解码不可变 metadata，完整契约进入 schema 身份。所有表示使用 CompleteBundle；
coordinator 对已关联、sealed 的字段执行有界分页校验，通过后才通知 observer、安装
共享结果或交付下游。动态 count 不改变 schema，空集合无需零 extent Value。
读取、work 与窗口由根预算约束，附加 work hook 不能替代根计账。范围和排除项遵循
[全局结果](Global-Results.zh.md)，不提供 RSS 认证。

## Spectrum 与 bands

Spectrum 保留原尺寸、变换轴、存储轴顺序、packing/packed axis、sign、norm、shift、
采样 origin/step、unit 和实输入策略。samples 为 Float64 complex pairs。R2CHalf
保留原尺寸奇偶且禁止 shift；被省略半谱由共轭定义。只检查实际存储的镜像关系，
Nyquist 整列不要求全实。ExactHermitian 直接比较有限实虚分量，避免模长溢出后乘零。
RealProjectionMeasured 的纯绝对容差在原量级比较；正相对容差分别保留差值与
容差的正常binary64尾数和指数，避免大相同分量抹去小差异或中间溢出。该 binary64
接受模式不构成认证变换误差界，inverse 必须明确 real projection 和 imaginary residual。

Bands v1 为复制末样本的 Haar，low=(a+b)/2、high=(a-b)/2，重建 low±high 后按
parent shape 裁剪。保存原尺寸、axes、filter snapshot、levels，以及每个显式 key 的
shape、parent、origin、step、phase、ExplicitZero。必需集合为最终 (L,0) 加每层全部
非零 mask。缺失、重复、显式零不同；不宣称一般 filter bank 完美重建。samples 按声明
顺序拼接实际成员，band_range 以 key 返回逻辑区间，显式零有逻辑形状但无存储区间。
五样本例由 low=[2,6,9]、high=[-1,-1,0] 重建并裁剪为 [1,3,5,7,9]。

## PathSet

坐标恰为两个有限分量，选择唯一几何来源。CoreVerbs 的 M/L/Q/C/Z arity 为1/1/2/3/0，
offsets 精确覆盖 controls，子路径首项 M，Z 只在末端并与 closed 一致。空路径的两个
offset 数组均为 [0]；单 M 是零长度开放子路径。

PrimitiveRef 含 tag、record index、payload field、所属 ObjectId；最后一个 Int64
宽度槽保存无符号 ObjectId 位模式。字段0..4为核心数组，5为 primitive refs，6为
Bezier，7为 arcs，8为 normalized Hermite，9为 spline records，10/11为knots/weights，
12为attribute定义，13/14为values/normalized arc positions。详细记录顺序见英文表。

Bezier 使用 Bernstein 多项式；Hermite 控制点等价于 P0,P0+d0/3,P1-d1/3,P1。
Arc 为 c+u cosθ+v sinθ，允许非正交轴；外向 determinant 区间必须证明非退化。
Sweep 为严格不足一周的非零 sweep，FullTurn 明确方向并复用起点；零、退化、多周
输入需显式转换，不靠 sin(2π) 判闭合。非周期 spline 检查 degree/controls/knots、
有效域和正有理权重；内部 degree+1 multiplicity 必须拆分子路径，允许 degree-zero
常数片段。右端取左极限，零分母项取零。

连接端点必须精确一致。Arc 与端点未知的非 clamped spline 可独立开放表示，FullTurn
可独立闭合；需要未证明端点吸附的混合子路径失败。未提供 periodic seam、拓扑或 AA
误差保证。属性 owner 为 Path/Subpath/Segment/Control/ArcLength，插值为 Constant/Linear；
跨度完整无重叠，ArcLength 参数严格覆盖 [0,1]。原长度变化可能影响整条归一化属性。

## 动态点、组件与 YCbCr

Points 包含 ids、positions、attributes 和按 ID 排序的 id_to_row；允许物理重排，
但映射必须完整双射。InputPosition ID 受固定 basis 约束，Ordinal 为0..count-1。
语义 maximum_count 与物理资源限额分离，零 count 合法。

Components 保存 labels 和 (id,area,min_position)。MinPixel ID=1+min_position，
CompactMinOrder 按最小位置编为1..K。校验精确计数、顺序、完整成员和 basis，
不以此证明任意导入 labels 的连通性。当前 validator 使用计费扫描/二分，work 不足时
有限失败。四连通 union recipe 和独立 BFS 由 labels/area/filter workflow 另外验证。

YCbCr420 v1 数据 schema 保留旧转换的固定表示。包 0.20.0 已移除 color.rgb_to_ycbcr420；
此 schema 不构成 I/O codec 实现。FMT-16 已退休，内核图像平面保持同尺寸 planar；
旧 schema 不允许将异尺寸平面作为 canonical 内核图像。历史字段为：BT.709 transfer/matrix、sRGB D65 primaries、
scene/display reference、Float32 full-range Y∈[0,1]、Cb/Cr∈[-.5,.5]。角色、重建和
box-clipped boundary 固定于 schema 版本。Chroma 为 ceil(H/2)×ceil(W/2)，名义
origin=(.5,.5)、step=(2,2) 与真实 clipped support 分开。3×5 最末名义中心(2.5,4.5)，
实际支持仅源像素(2,4)。

## Brush 与 iteration

Brush state_ids 保存 stroke、next/final event、final dab count、初始/当前canvas版本、
generation、seed、counter、end。state 为 last position、remaining、P[3]、A、E[3]。
pending有界且ID/position计数一致，end要求无pending。已终结dab保持有序、正spacing，
并与carry phase一致。binary64一致性容差为32 epsilon乘相关位置/spacing最大绝对值，
再加32最小subnormal单位；这是校验模式，不是渲染 CertifiedBound。A∈[0,1]，A=0时P=0，
E独立且有限有符号。

advance_causal_brush 计算一维恒定spacing fold，跨batch保留phase，返回根计账dabs
和新state；dab向量使用Payload allocator角色，复制仍受payload子限额约束，增长计入
旧新块同时存活。失败不修改旧state，不能前进或fuel不足均失败，不截断成功。表示保留lookahead/
pending，但helper明确只计算因果模式，不扩展为 smoothing/smudge。

Iterative 字段为(generation,iteration_count,stop_reason)、estimate、diagonal、rhs、
measured infinity residual。StopReason为Converged=0、IterationLimit=1。系统snapshot、
初始化、size、iteration cap和收敛要求进入schema。zero初始化要求x0=0。validator 用独立
product舍入重算 max(abs(b-rounded(a*x))) 并检查stop政策。approximate必须显式选择，
有限/零残差均不自动形成CertifiedBound，已发布ResultRef冻结estimate和关联数据。

## 公开验证

构建、focused CTest 和实际安装 consumer 命令见英文文档。示例使用内存测试wire，
不是持久文件格式：16个小端count后接宿主primitive字节。staged producer读取count，
按页写入必需backing；真实Result下游读取descriptor。context结束后逐字段与独立fixture
字节比较，并用明确参考检查奇数Haar与brush batch。可用首个CLI参数选择打印出的fixture名。

iteration-resume-generation 将零初始化 approximate Result 经分页对角step送入新收敛
Result及活跃下游，检查generation 1→2、iteration 0→1、解[3,4]、旧x0不变，以及
context结束后派生结果继续持有前序数据。

数值schema/内容校验及causal brush helper建立默认nearest/gradual-underflow环境，
随后恢复调用方浮点状态；环境无法建立时返回operational failure。
