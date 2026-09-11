# ADR 0021：独立节点结果与 Atomic 联合执行

- 状态：维护者于 2026-09-12 接受
- 交付：[#302](https://github.com/kevin-zf1123/photospider/issues/302)，叶项 #303–#313
- 决策基线：`ops@ffc5d0e297b9d0ea136975413d3443e23e6fa458`，包 0.8.0 / operation ABI 8

## 背景与证据

当前编译器仅接受 value 输出端口，并按节点索引 metadata 与 step。执行记录和部分
缓存也假定一个节点只有一个结果。PerAtomOutcome 虽有声明，但注册拒绝它；Atomic
分阶段 session 只允许一个 observation。不可变 Value、精确 Footprint、关联证书、
独立 flight waiter 和存储 owner 计费继续作为基础。这些源码事实不代表新能力已实现。

[MLIR operation 与 value](https://mlir.llvm.org/docs/LangRef/#operations)供结果身份设计参考。
[ITU-R BT.709-6](https://www.itu.int/dms_pubrec/itu-r/rec/bt/r-rec-bt.709-6-201506-i!!pdf-e.pdf)
定义下述 OETF 与 YCbCr 矩阵；box、边缘策略与浮点表示是 Photospider 的明确选择。

## 结果、推导与版本

一个 Value 具有单一 dtype、非零静态 shape 和坐标域。节点包含有序且非空的独立命名
输出，以 OperationOutputTraits 和 SemanticOutput 表达。ValueRef 为
(node_id, output_index)。名称唯一，索引按声明顺序；单输出显式声明 value。
每输出分别声明 metadata、相关输入、Region、观察与失败契约。推导包含检查溢出的
ceil-div、减法和非整数半径 extent，禁止执行数值回调求 shape。

Semantic IR 原生多输出；每个有需求的结果降低为单输出 PlanStep。未选择的纯输出不
产生需求；Empty、Whole 和尚未解析的集合相互区分。既有单输出副作用保持；多输出
要求确定性、无外部副作用。RequestRecord 保持完整请求的终端结果。消费者合法性和
EffectiveAtomic 检查所选结果及其相关祖先，不受无关兄弟结果影响。

C/C++ 调用携带所选结果。输入投影保留原端口索引，不使用 invalid Value 填空。
记录、订阅、dirty、Whole 结果、诊断和 flight 区分结果。内容键包含输出、契约、
metadata、静态参数及实际输入依赖，不包含全局节点或图 id。未知证书行不表示空依赖；
读取并集不能取代每输出的 Data/Control/Validation/Descriptor 关联。

目标为包 0.9.0、operation ABI/traits 9，不提供 ABI 8 适配。自有回调和安装消费者
共同迁移。WorkflowDocument schema 2、provider ABI 1、C++17、image-v2 完整像素语义
保持。semantic/physical-plan/plan-cache 域升级 v9，result-region 升级 v5；未变的保守
optimizer 与 result-digest framing 保持版本。旧临时缓存失配，不建立恢复产品。

## Atomic 联合执行

在 singleton 入口外增加可选 joint 入口，执行分组属于物理选择。每组每输出最多一个
Atomic observation，允许不同 shape、坐标和 ROI；RequestRecord 不参与。登记所有已知
根需求及新发现的输入需求后，选择同节点、snapshot、backend、joint contract 的就绪
成员。无需等待未来请求，不跨 Run 合组；继续通过逐 observation flight 跨 Run 共享。

C/C++ 各成员独立返回读取、成功值或错误。每个选定 observation 恰有一个终态；缺失、
重复、未知成员及越界结果均拒绝。成员保留独立 sticky 状态、关联行和 token。仅传输
去重；各成功结果独立验证证书、完成 flight 和发布缓存。数值失败只传播到实际依赖者。
公开 execute 保留原请求的整体成功/错误聚合及确定性错误顺序。

缺少兼容 joint、就绪成员少于两个或联合预算不足时使用 singleton。无法归属成员的
执行失败先释放联合临时资源，再将未完成成员各以 singleton 重试一次，禁止重新合组。
取消、失效及结构协议错误不重试。所有成员均无活跃 waiter 时共享组才停止；一个成员
取消不覆盖其他活跃请求。组具有一个 admission slot 和共享 scratch reservation；真实
存储按 backing owner 计费，直到最后一个 view/cache/result 引用释放。

## 可复用算子

- color.rgb_to_ycbcr420：有限 [0,1] Float32 线性 sRGB RGB、无 alpha，先 OETF 后矩阵。
  Y 为 HW；Cb/Cr 为 ceil(H/2)×ceil(W/2)，有符号色差名义范围 [-0.5,0.5]。使用居中
  2×2 box，奇数边缘只平均有效样本。平面角色、颜色和名义采样位置显式表达，与数值支持分离。
- image.split_horizontal：full/left/right，0 < split_x < W，独立 ROI 原点，可行时共享
  不可变存储视图。
- image.convolve_channels：HW r/g/b、独立 Float32 HW kernel 和整数 anchor，支持奇偶
  非对称核和 zero/clamp 边界。每输出读取自己的整个核及局部图像邻域；区域化
  field.convolve 支持导出核复用。
- image.gaussian_blur_with_kernel：图像及参数推导的 Float32 kernel。radius/sigma 为
  有限 Float64 [0,64]；R=ceil(radius)，shape=(2R+1)^2。整数偏移处
  a(d)=clamp(radius+1-|d|,0,1)，归一化 exp(-(x*x+y*y)/(2*sigma*sigma))*a(x)*a(y)。
  sigma=0 为同尺寸中心冲激；整数半径是普通截断权重，非整数半径连续引入边界权重。
  radius=1.25 为 5×5，每轴外层因子 0.25。图像使用导出的相同系数、固定行优先 binary64
  累加；单请求核不读取图像样本，联合请求共享生成；既有 Gaussian 语义保持。

## 验收与交付

#303–#313 依次实现契约、ABI、编译器、执行身份、PerAtomOutcome、联合调度、平面/420、
裁剪、通道卷积、高斯输出和安装 workflow，每项独立验证并提交。
examples/multi_output_workflow 必须通过安装的 static/shared 公开 API 运行，具有独立
数值 oracle、源支持集合和回调计数。

覆盖 singleton/joint、未选择兄弟输出、同 shape 缓存隔离、不同 ROI/tile/顺序、控制编辑、
未知行、晚到 dirty 增量、冻结快照、混合 hit/flight、waiter 取消、过期发布、共享 owner
释放及预算回退。radius 覆盖 0、0.25、1、1.25、2、64、整数相邻浮点及非法非有限参数。
C fixture 检查结构大小、计数、coverage 和生命周期。执行 focused 本地验证及现有六项 CI，
不增加流程文本测试。

全部叶项后启动新的独立全面审查，完成 PR 到 ops、必要修复、最终 HEAD CI 和 Codex bot
review。merge commit 保留叶项历史。确认 ops 合并后显式结算 Issue、同步本地 ops、仅删除
本轮分支。不隐含 main 合并、daemon 迁移、动态输出长度、新 Metal 算法或 #206 完成。
接受决策不等于实现或交付完成。英文 ADR 为权威，本文件为阅读镜像。
