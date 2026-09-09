# S3：可编辑的缓存图像 workflow

独立 [示例](../../../examples/s3_image_workflow) 只使用安装后的公开 API，组合不可变
输入快照、Gaussian blur、运行期曝光、独立蒙版、source-over，并提供硬边圆章和
box 缩小预览。默认图像为 17×13、线性 sRGB 预乘 Float32 RGBA，蒙版为 HW Float32。

## 构建与运行

在仓库根目录安装已构建内核，再独立配置示例，无需 daemon：

```sh
cmake --build build/issue257-static --target photospider -j 8
cmake --install build/issue257-static --prefix build/s3-prefix
cmake -S examples/s3_image_workflow -B build/s3-example-installed \
  -DCMAKE_PREFIX_PATH="$PWD/build/s3-prefix"
cmake --build build/s3-example-installed -j 8
build/s3-example-installed/photospider_s3_image_workflow --scenario cache
build/s3-example-installed/photospider_s3_image_workflow --scenario preview
```

plugins/ops/rgba32f 可独立使用同一 prefix 构建；通过 --module PATH 指定可信模块，
在 C operation ABI 路径运行 cache 和 preview。磁盘持久复用目前要求具有可核验
指纹的内建 registry；自定义/C 模块 registry 支持进程内结果缓存。

磁盘验收使用专用目录，每条命令均启动独立进程；损坏或清空后从相同输入重建：

```sh
build/s3-example-installed/photospider_s3_image_workflow \
  --scenario disk-write --cache-dir "$PWD/build/s3-derived-cache"
build/s3-example-installed/photospider_s3_image_workflow \
  --scenario disk-read --cache-dir "$PWD/build/s3-derived-cache"
build/s3-example-installed/photospider_s3_image_workflow \
  --scenario disk-corrupt --cache-dir "$PWD/build/s3-derived-cache"
build/s3-example-installed/photospider_s3_image_workflow \
  --scenario disk-clear --cache-dir "$PWD/build/s3-derived-cache"
```

## 可检查结果

| 场景 | 预期观察 |
| --- | --- |
| cache / S3Cache.LocalInvalidation | blur_after_gain=0 patch_blur_tiles=6 unrelated_callbacks=0 oracle=passed |
| preview / S3Preview.LatestAndExport | stamps=9 export_tiles=20 preview_tiles=27 rejected=3 quality=full oracle=passed |
| disk-write | oracle=passed，退出后保留有效条目 |
| disk-read | 磁盘命中数大于零，oracle=passed |
| disk-corrupt | 无效条目数大于零，重算后 oracle=passed |
| disk-clear | 清空重算后仍通过独立 oracle |

圆章输入使用独立顺序公式检查；正式图像使用整图二维 Gaussian/合成 oracle，容差
atol=1e-6、rtol=1e-5。代理明确为近似，正式结果与导出保留原模糊参数。计数是确定
工作量，不构成时间承诺或请求到屏幕延迟测量。

## 修改与组合

scene.hpp 定义 workflow 与输入，可修改 fixture、算子连接、静态 radius/sigma 和
代理因子。四分之一代理先缩小图像/蒙版，再模糊合成；半径按比例向上取整，至少 1，
sigma 按比例缩小，至少 0.1。gain、圆心、半径、颜色、alpha 及新像素快照是运行
绑定，复用已编译计划；图编辑重新编译文档。FrozenExecution::for_region 不重新
分析就能派生区域工作。

InputSnapshotStore::import_value 建立不可变块；patch 返回新版本并共享未变块。
ExecutionBinding 在 Value、RegionalSource、InputSnapshot 中选择一项。冻结接受
Value 与内核快照，自定义 RegionalSource 需先导入。快照容量独立限额，包含旧版本。

coordinator.hpp 是应用策略，不是安装后的请求/Job API。单线程事件循环保留 8 项
圆章 FIFO 和合并后的待处理 gain。队列满时圆章可在推进后重试。每 tick 至多应用
一个编辑并执行一个预览或导出 tile；两者活动时交替服务。已接纳圆章保持顺序。
预览完成检查目标、内容版本和质量；导出持有初始冻结输入。应用组装的 frame 缓冲
区不计入内核受控预算。

设置 ExecutionContextConfig::result_cache_bytes 启用内存复用，默认零。缓存保留
仍位于 maximum_live_bytes 内。示例受控执行预算 1 MiB，缓存子限额 256 KiB。
DiskCacheConfig 显式指定独占目录和有限额度，异步写可跳过。flush_disk_cache 只用于
显式磁盘交付，不进入预览发布路径。清空两类缓存均不影响重算正确性。

## 验证边界

测试还覆盖奇数尺寸、图像/蒙版缩放、参数错误、局部 patch、旧快照、普通 stale、
冻结导出、独立及最后订阅者取消、清空竞态和严格容量。独立磁盘进程覆盖头/长度/
摘要/版本损坏、写失败、驱逐和队列压力。静态/共享安装消费在 C++ 与 C 路径运行
示例。本轮不包含原生 GPU、GUI、压力动态、文档保存、持久 Job 或 daemon 新协议。
