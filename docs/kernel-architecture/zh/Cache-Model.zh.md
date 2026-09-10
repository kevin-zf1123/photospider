# 缓存模型

PlanCacheKey 仍是非安全物理计划身份，不包含输入像素，也不能验证过期计划或标识
执行结果。

package 0.7 通过 ExecutionContext.result_cache_bytes 显式启用结果保留，它是
maximum_live_bytes 的子限额。副本共享不可变分配租约；驱逐只释放缓存引用，严格
工作集接纳前优先回收可选条目。clear_result_cache() 推进保留 epoch，活动读取者
仍有效，旧生产者不能重新填充已清空 epoch。统计报告命中、未命中、驱逐、共享、
条目数和容量。零缓存额度保留无缓存执行。

InputSnapshotStore 独立限额管理 rank 1–8 的 UInt8、Int64、Float32、Float64 不可变块。
Generic Value 保留所有合法原始位模式；typed 导入与 patch 校验各自的语义样本规则。
Image-v2 RGB/RGBA/XYZ/Lab 保留有序通道角色、参考白、单位和 alpha association，始终
校验并复制完整像素通道。配置的 block_size 用于 generic 的每个轴及图像 H/W，图像 C
保持完整。maximum_blocks 在像素分配前限制每个版本的目录条目；maximum_bytes 统计
所有版本仍持有的实际块。目录元数据单独有界，该额度不是进程 RSS 上限。

Patch 要求 dtype、shape、facets 完全一致，复制相交块并保留旧版本。
SnapshotAccessOptions 提供取消与样本上限，约束导入、读取、hash 以及 patch 的受影响
块复制。取消的 read 可能已经部分写入调用方缓冲区，只有成功返回才确认其内容。
content_identity(region) 使用 photospider.input-region.v2 域及规范 SHA-256，包含 dtype、
shape、需求坐标、facets 和精确样本位。整数/IEEE 样本按实际宽度解码，再编码为
uint64 little-endian 字段；分块、origin/stride 和分配地址不进入身份。正负零及 NaN
payload 保持不同。包括 generic 输入在内，快照绑定的区域读取传递 Run 取消令牌。

内存和 native 完成值保留真实 facet。结果命中在复用前核对解析后的 descriptor、需求
coverage、输出语义规则和 typed 样本约束，包含 flight 完成竞态中的二次查询。数值验证
建立 nearest/gradual-underflow 并恢复调用方浮点环境。Generic Drop 输出可发布 opaque
facet；由该动态边界开始的 PreserveInput 链保持此能力，仍拒绝未推断 typed facet。
已知 declaration 和显式输出语义要求精确 facet 相等。非法计算
typed 值仍为 OperationFailed。输入副本键已包含完整元数据，native 发布保留这些信息。

result-region v3 键递归覆盖各消费者实际需要的上游区域、算子语义/参数和稳定输入内容。无关
图编辑及节点编号不影响相同内容。Whole 依赖保守；标量变化影响相关输出。未证明
稳定的通用输入仍可执行，其后代不跨 Run 复用。只缓存确定、无副作用、cacheable
的 CPU 工作。

区域结果 key 也接受经过 preflight 的 dense、offset-zero 直接 Value：最多 2048 bytes，
派生 demand 必须覆盖完整 Whole 值。Snapshot、bounded-scalar、compact Whole Value 有
独立类别 tag；compact key 包含 dtype、rank/shape、精确 facet、byte 长度与原始 bytes，
包括负零和未使用系数。更大或局部直接输入无资格。该规则用于区域执行与 execute_stream；
纯 generic/scalar 的普通 execute 保持既有快速路径；磁盘结果仍仅支持既有图像/蒙版。公开
expression workflow 检查 2048/2049+ 边界、dtype/shape/facet 分离、并发系数及缓存非法
bounded 消费者拒绝。

有界协调线程合并相同在途区域计算；CPU 工作仍在原回调池。调用方分别观察取消与
图当前性。最后一个订阅者取消后，等待生产者退出才返回。生产者快照独立持有输入
和 registry，不依赖调用方栈或可编辑图。

FrozenExecution 是内存中的拥有者对象，无序列化 plan reader；捕获当前计划及不可变
Value/快照绑定，允许原图替换/销毁。捕获复制快照句柄，调用方之后替换句柄不改变
冻结输入。普通
执行仍检查 stale。for_region 派生冻结输出 tile。自定义 RegionalSource 须先导入
再冻结。

## 可丢弃磁盘区域

显式 ExecutionContextConfig::disk_cache 要求正结果缓存容量。DiskCacheConfig 指定
目录、总字节/条目上限和有界写队列；一个 context 独占锁定目录。只管理 SHA-256
文件名的 .pscache 与可丢弃 .tmp，忽略无关名字。磁盘容量包含活动写入预留；待写
结果继续持有原受控分配，计算压力下可丢弃待写队列。

持久复用限定 make_default_operation_registry()；指纹覆盖维护源码/头文件、编译器、
平台和构建参数。自定义与 C 模块 registry 支持进程内缓存，不发布持久实现身份。
该身份用于正确性，不构成原生代码信任或安全签名。

磁盘格式 2（PSCACHE2、disk-result key domain v2）保存 Float32 HW coverage 蒙版或
受支持的 HWC image-v2 区域。头编码 dtype、shape、Region、精确 canonical facet 的
key/version/payload、字节数和 key；SHA-256 覆盖头与 packed little-endian Float32
样本位。旧格式 1 为 miss。读取比较完整预期头，发布已验证 facet 并复验 typed 样本，
不重建默认 RGBA facet。分配尺寸来自已验证计划，不来自文件
长度声明。头、尺寸、摘要或数值失配均作为 miss。临时文件完整后 rename，不提供
持久提交/恢复保证。写入失败或队列压力跳过保留，不使计算结果失败。
flush_disk_cache() 是发布路径之外的显式等待；销毁也等待写线程。
clear_disk_cache() 删除条目并作废待写 epoch。

test_disk_cache 使用独立进程覆盖写入、复用、头/长度/摘要/版本损坏、删除重建、
写失败与严格配额/队列压力。test_input_snapshot 和 typed disk 回归覆盖 RGB/BGR、
straight/premul RGBA、XYZ/XYZA、Lab/LabA，包含 D65/D50 元数据隔离、三通道 patch、
冻结旧输入、冷/热复用和重启。tests/support/typed_images.hpp 提供公开
WorkflowDocument/compile/execute identity 示例与可检查的 signed/HDR/负零样本。
参见 ADR 0018。

## S4 原生驻留

MetalFp32 结果键增加数值模式、计划后端、编译实现身份和 context 设备代次。
回退结果及后继不写预期原生结果键；冻结 registry 保持 C 模块实现所有权。
CPU 精确缓存与原生近似结果隔离；设备失效停止原生键并清理条目。

完成的原生输入副本共用有界 LRU，按实际需求样本、描述符、facet、Region 和设备
生成身份，不使用可复用分配地址或调用者 revision。普通不可变 Value 也可以避免
重复上传；任意 RegionalSource 仍须重新读取实际字节，不因此获得计算结果缓存资格。

native_retained_bytes 是 retained_bytes 内唯一原生 owner 容量，native_upload_hits
记录避免的上传。活跃读者、缓存、输入副本保持原始预算 lease。clear/eviction 只
移除资格；共享工作保留独立/最后订阅取消，follower 不重复计数 producer 的原生工作。

本实现磁盘读写仅允许 CpuExact，即使 Metal 请求回退也不写磁盘。CPU 磁盘行为保持。
原生源码和 shader 输入进入实现身份，生成头留在构建目录。test_native_cache 及 C
插件版本验证复用、编辑、模式/回退隔离、取消、clear race 和保留容量。

使用既有构建目录执行 focused 验证：

```sh
cmake --build build/issue257-static --target test_input_snapshot test_disk_cache test_result_cache test_frozen_execution test_native_cache -j 8
MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 ctest --test-dir build/issue257-static -R '^test_(input_snapshot|disk_cache|result_cache|frozen_execution|native_cache(_plugin)?)$' --output-on-failure
```

Typed native 驻留使用公开纯字节复制操作：九种描述符均须保留精确 bytes/facets，冷运行
一次 dispatch，缓存命中零 dispatch。此处验证存储与复用，不新增颜色转换算子。
既有 native 取消与预算用例继续保留。
