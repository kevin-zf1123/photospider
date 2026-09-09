# 缓存模型

PlanCacheKey 仍是非安全物理计划身份，不包含输入像素，也不能验证过期计划或标识
执行结果。

package 0.5 通过 ExecutionContext.result_cache_bytes 显式启用结果保留，它是
maximum_live_bytes 的子限额。副本共享不可变分配租约；驱逐只释放缓存引用，严格
工作集接纳前优先回收可选条目。clear_result_cache() 推进保留 epoch，活动读取者
仍有效，旧生产者不能重新填充已清空 epoch。统计报告命中、未命中、驱逐、共享、
条目数和容量。零缓存额度保留无缓存执行。

InputSnapshotStore 独立限额管理不可变图像/蒙版块。导入校验 profile；patch 只复制
相交块并保留旧版本。content_identity(region) 使用规范 SHA-256 对元数据与需求样本
位求身份，与分块尺寸和分配地址无关；有符号零保持不同。快照绑定提供区域读取。

结果键递归覆盖各消费者实际需要的上游区域、算子语义/参数和稳定输入内容。无关
图编辑及节点编号不影响相同内容。Whole 依赖保守；标量变化影响相关输出。未证明
稳定的通用输入仍可执行，其后代不跨 Run 复用。只缓存确定、无副作用、cacheable
的 CPU 工作。

有界协调线程合并相同在途区域计算；CPU 工作仍在原回调池。调用方分别观察取消与
图当前性。最后一个订阅者取消后，等待生产者退出才返回。生产者快照独立持有输入
和 registry，不依赖调用方栈或可编辑图。

FrozenExecution 捕获当前计划及不可变 Value/快照绑定，允许原图替换/销毁。普通
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

有限格式保存 Float32 HW 蒙版或 HWC RGBA 区域、版本、精确预期元数据/键，以及
元数据和 little-endian 样本位的 SHA-256。分配尺寸来自已验证计划，不来自文件
长度声明。头、尺寸、摘要或数值失配均作为 miss。临时文件完整后 rename，不提供
持久提交/恢复保证。写入失败或队列压力跳过保留，不使计算结果失败。
flush_disk_cache() 是发布路径之外的显式等待；销毁也等待写线程。
clear_disk_cache() 删除条目并作废待写 epoch。

test_disk_cache 使用独立进程覆盖写入、复用、头/长度/摘要/版本损坏、删除重建、
写失败与严格配额/队列压力。参见 ADR 0018。
