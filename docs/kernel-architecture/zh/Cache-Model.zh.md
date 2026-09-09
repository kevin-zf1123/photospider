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

磁盘缓存契约见 [ADR 0018](../../adr/0018-local-result-caches-and-frozen-execution.md)，
实现及重启验收由 #276 跟踪。
