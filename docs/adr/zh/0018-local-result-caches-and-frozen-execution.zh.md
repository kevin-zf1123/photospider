# ADR 0018：本地结果缓存与冻结执行

- 状态：Accepted
- 日期：2026-09-09
- 接受：维护者明确批准完整 S3 计划、以下选项及逐 Issue 提交、独立审查、受保护 PR 交付和清理。
- 基线：kernel `54d57f30a68807c4d09fbc7439debda0a9b0c614`
- 权威原文：[English](../0018-local-result-caches-and-frozen-execution.md)

接受确定目标，Issue 记录实际交付。本决策只替换 ADR 0015 对本地磁盘派生 Value
的排除、显式冻结执行的图当前性要求，以及 ADR 0017 的算子/traits 版本与同尺寸
区域映射条款。应用拥有预览请求、事件队列和发布；内核拥有输入、执行、共享计算
和可丢弃缓存。不加入 daemon Job/IPC、文档保存、恢复、artifact 权威、原生 GPU
或增量编译产品。

## 研究与选择

[libvips evaluation](https://www.libvips.org/API/8.17/how-it-works.html) 和
[tilecache](https://www.libvips.org/API/8.17/method.Image.tilecache.html) 展示按需区域
生成、有界 tile 保留和下游失效。
[OpenImageIO ImageCache](https://github.com/AcademySoftwareFoundation/OpenImageIO/blob/main/src/include/OpenImageIO/imagecache.h)
采用近似容量限制；本实现继续严格遵守 S2 受控分配上限。
[libvips shrink](https://www.libvips.org/API/8.17/method.Image.shrink.html) 提供 box
滤波比较。不引入框架依赖或时间性能声明。FNV PlanCacheKey 不包含像素，不能标识
运行结果；#203 仍只复用编译片段。

整图身份会使无关工作失效；调用方版本声明无法验证可变数据。选择内核不可变分块
快照和精确需求内容身份。选择单一 breaking ABI 5，避免双套区域校验。应用请求
策略放在内核之外。可丢弃条目无需事务数据库恢复。

## 版本与缩放区域

软件包 0.5.0、OperationTraits 5、operation C ABI 5 替换 0.4/4。C++17、源 schema 2、
provider ABI 1 保持。C++ 消费者重建；拒绝 ABI 4，不提供适配。变化的编译语义使用
v5 摘要域。运行内容使用独立 SHA-256 域，保留参数和样本位模式，包括有符号零。

显式 box 缩小规则解析必需的 [1,16] 整数参数。输出 H/W 为输入除以因子向上取整，
保留完整通道。反向需求乘因子后裁剪图像边缘；正向脏区使用 floor/ceil 除法映射。
算术检查溢出。图像和蒙版各自保留 profile。边缘按实际样本数平均，累加顺序固定
为行/列。通用 Whole 保持保守语义。

## 输入与执行快照

公开快照存储导入有效 Float32 RGBA 或蒙版，持有不可变块；精确区域替换 patch
只复制受影响块，提供区域读取和身份。旧版本可读，未修改块共享。像素存储设置
独立字节上限；失败保留原快照并返回 ResourceExhausted。元数据不计入像素预算。

显式冻结执行捕获当前有效且匹配的计划、不可变绑定及 registry 所有权，允许原图
替换或销毁。普通 execute 保留 stale/cancel 优先级。冻结执行检查取消与自身有效性，
不使用后续输入版本，并在回调全部退出前保留所有者。

## 结果缓存与共享计算

ExecutionContext 显式启用缓存。键覆盖局部算子语义、参数位、按序需求输入内容与
元数据、输出 Region、后端及实现身份。整图修订和无关分支不进入局部键。未证明
稳定的通用输入仍可执行，但相关结果不能跨 Run 复用。只缓存明确 cacheable、确定、
无副作用的路径；标量绑定按不可变 Value 的精确字节标识。

正向失效遵守 Elementwise、Halo、缩小和 Whole 依赖。改曝光保留 blur；局部像素
只影响依赖需求区域；无关图编辑不破坏复用。清空和驱逐均允许重算。精确内容身份
授权复用，不能只凭脏区提示。

同一 ExecutionContext 的相同在途计算共享一个生产者；订阅者独立取消，一个取消
不影响其他订阅者，最后一个退出时请求停止。生产者保留冻结输入和 registry，直到
已接纳回调全部退休。回调不得等待占据同一池的另一回调。只有成功校验的不可变
结果可以进入完成缓存。

缓存容量是 maximum_live_bytes 子限额，共享分配只计费一次。驱逐或销毁 context
后，调用方保留结果仍持有容量租约。接纳计算前回收空闲条目，不无限等待调用方
释放。无法保留缓存则跳过，最小工作集无法容纳则 ResourceExhausted。分别报告命中、
未命中、驱逐、共享计算、驻留字节和实际工作量。

## 算子与应用 workflow

C++ 与维护的 C 模块提供图像/蒙版 box 缩小及 image.brush_circle。圆心、正半径、
非负有限线性 RGB、[0,1] alpha 为运行输入。闭圆内像素中心使用预乘 source-over；
圆外保持原像素。一个事件一个硬边圆章，不补点，不处理设备动态。只执行裁剪后的
圆包围 Region，再按序发布 patch。

应用先执行四分之一分辨率代理，再执行全尺寸。代理 blur 的半径/sigma 按比例
缩放并限制合法范围，结果明确为近似；正式导出保留原参数。有界队列合并待处理
滑块预览，已接纳笔画不丢失。满时背压允许重试。有限 tile 批次交替服务预览/导出。
发布检查内容版本、目标、质量；拒绝过期与质量下降。冻结导出始终读取原始输入。

## 可丢弃磁盘数据

嵌入方显式指定独占本地目录、字节和条目上限。内核只保存有限 CPU Float32 图像/
蒙版区域，使用版本化未压缩规范字节表示。发布前校验描述符、facets、Region、
检查后的长度、键和 SHA-256。持久复用要求可验证的维护实现指纹；实现或构建变化
导致 miss。未核验的外部实现只允许符合纯函数要求的进程内复用。

临时文件完整后才发布。残缺、损坏、未知版本和失配条目丢弃重算。有界异步写入
遇失败或压力跳过缓存，不进入必需的预览发布路径。重启只复用派生数据，不恢复
工作状态、请求、文档或输出权威。不声明持久提交保证。一个目录同时只有一个
活动所有者。

## 验收与交付

S3Cache.LocalInvalidation 比较缓存开关输出及曝光、圆章、无关分支变更后的精确
工作区域。S3Preview.LatestAndExport 在冻结导出期间回放编辑，验证顺序、边界、
进度及发布仲裁。S3Disk.DiscardAndRebuild 使用独立进程，损坏、删除、驱逐后比较
独立正式结果。

使用确定性同步覆盖奇数尺寸、边缘、halo、透明/HDR、无效参数、旧快照、取消/
清空竞态及精确/不足预算。examples/s3_image_workflow 使用安装公开 API。验证静态/
共享 C/C++ 和 daemon 0.5 消费。逐叶提交；独立全面审查及 CI/bot 修复后受保护合并
并结算 Issue。

## S4 目标修订

[ADR 0019](0019-metal-resident-image-workflows.zh.md) 增加显式 Metal 执行、原生 shared storage 和 operation ABI 6。接受定义目标，不代表实现完成；其余边界保持。
