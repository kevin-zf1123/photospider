# ADR 0019：Metal 驻留图像工作流

- 状态：Accepted
- 日期：2026-09-09
- 接受：维护者明确要求实施完整 S4 计划，包括以下决策、逐 Issue 提交、独立审查、受保护交付和清理。
- 基线：kernel `0e65eac`，daemon `8816f85`
- 权威英文：[ADR 0019](../0019-metal-resident-image-workflows.md)

## 研究与范围

S4 交付一个 Apple Silicon Metal 后端，覆盖内置与独立 C 算子包中的八个图像算子。
CPU 必需。使用显式选址，#209 实测成本与自动选址留到 S5。其他 GPU 后端、#203、
GUI、daemon 协议扩展、远程设备和持久 GPU 缓存不在范围内。

基线 GPU lane 执行同步主机回调，按后端标签复制主机字节，没有原生资源；结果键
拒绝 GPU 上游。既有测试只证明调度。Apple shared storage 文档允许 CPU/GPU
共享访问，waitUntilCompleted 等待命令与完成处理器。M5 实机探针关闭 fast-math 后，
位型 2 乘 .5 的 GPU 结果为 0、CPU 为 1；最小 normal 乘 .5 的 GPU 结果为 0、CPU
为 0x00400000；double shader 编译失败。探针只证明可行性，不构成产品验收。

## 公开版本与身份

package 0.6、operation ABI 6 和 traits 6 替换 0.5/5，拒绝旧表与旧 minor 请求，
C++ 消费者重编译，不保留兼容别名。C++17、文档 schema 2、provider ABI 1、IPC v3
保持。计划选择 CpuExact（默认）或 MetalFp32。数值模式与原生实现能力影响物理身份，
trait 编码变化影响语义身份；运行句柄、分配地址、耗时与设备代次不进入语义身份。
不新增优化规则，程序字节与编译选项标识原生实现。

## 原生存储、访问与完成

ExecutionContext 拥有实际可选设备、一个队列、pipeline 复用及同步 GPU lane。
只在受支持 Apple Silicon 上启用；关闭构建选项或设备不可用时保留 CPU。
每个 context 同时最多执行一个原生提交，等待命令完成后回调才退役，不新增异步
回调 API 或无限设备提交队列。

CpuStorage 表示不可变、CPU 可访问的存储，可以拥有 Metal shared buffer。
Value 布局、origin、Region 和描述符独立于分配。发布后的 bytes 立即可读，结果
持有原生 owner 和预算 lease 时可以超过 context 生命周期。输出与 scratch 在完成
前不得发布、释放或复用。原生输入副本、输出、scratch、暂存的实际容量计入现有预算，
共享分配计算一次，分配前预留，缓存是子限额；无法容纳的工作集明确失败。
驱动元数据、pipeline 和调用者既有输入不属于像素 buffer 分配，有观察能力时单独报告。

计划显式表示上传、算子、主机访问边界，包含类型化 producer、Region/layout 与字节
界限。GPU 后继复用 buffer；完成后主机访问 shared buffer 不虚构 D2H 复制。
分别报告复制数/字节、dispatch、设备完成时间与主机验证。

## 可信纯 C GPU 服务

ABI 6 保留同步回调，增加调用内 buffer token、有界输入/输出/scratch view、MSL
程序与入口、buffer bindings、常量和 dispatch grid。宿主验证记录并管理编译、提交，
原生完成后才返回。插件不创建设备/队列、不保留 token、不向 SDK 暴露 Objective-C。
shader 为可信进程内代码，校验不提供沙箱。C11 rgba32f 和内置八个算子使用相同服务。
CPU 回调不能宣称完成 Metal 工作，两条注册路径均执行既有输出、数值、取消检查。

## 数值模式与回退

CpuExact 保持既有 CPU 算术及缓存语义。显式 MetalFp32 关闭 fast-math 和 contraction，
每算子及指定代表链对独立 CPU oracle 使用 atol=1e-6、rtol=1e-5。任意组合会传播误差，
不承诺与图规模无关的总误差或 CPU 位相同。

Gaussian 在宿主 double 生成系数，GPU 补偿 Float32 累加；box 使用补偿累加与边缘
实际样本数。圆章以宿主 double 几何生成精确行区间，GPU 计算颜色，圆外保留位型。
不适用的 HDR、subnormal 或其他数值情况在发布前回退 CPU，非法输入保持既有错误。

回退粒度为算子。Metal 链内 CPU 回退消费实际输入，不使整条链成为 CpuExact。
后端/数值拒绝只在发布前、已提交工作排空后重试；设备执行错误终止 Run，设备失效
使驻留代次无效。取消停止新 admission，排空已提交工作并拒绝发布；stale/frozen
规则保持。

## 驻留与 S3 集成

同一 context 可保留原生输入副本和成功结果。键包含可证明的不可变内容、Region、
实际实现、数值模式与设备代次。不可信内容身份的 source 可执行，但不跨 Run 复用。
dirty mapping 引导需求，精确内容身份授权复用。

CPU 精确结果与 Metal 派生结果隔离。发生回退的算子及其后继不写预期 Metal 条目，
GPU 上游不进入 S3 磁盘缓存。清理、驱逐、设备失效移除复用资格，但不提前释放活跃
owner。共享计算保持独立订阅取消语义，成功验证且有效的结果才进入完成缓存。

## 验收与交付

独立安装示例 examples/s4_gpu_workflow 提供 resident-chain、all-operations、
cache-edits、preview-export、fallback，支持 CPU/Metal、内置/C 插件及 whole/tile/ROI。
非空原生验收必须观察到真实 dispatch。覆盖 halo/边缘、非整除、八算子、数值极端与
圆章精确覆盖、复用、设备失败、取消/stale、预算和结果寿命。工作量与资源确定性断言，
耗时不设通过阈值。

要求 static/shared 安装 C/C++ 消费者和 daemon 0.6 迁移。CI 保留既有必需任务；
无设备测试明确 skip，不宣称 GPU 通过。逐 Issue 提交后独立全面审查，修复 CI/bot，
按 kernel 再 daemon 合并，结算 Issue/Project 并清理分支。

## 替代条款

仅替代既有 CPU 存储/原生驻留限制、operation/traits/package 目标版本，以及显式
Metal 模式下无条件精确数值等价要求。ADR 0015 产品归属、0016 CPU 输入、0017
Region/预算、0018 snapshot/cache/frozen 归属继续有效。接受是目标，Issue 记录实测交付。
