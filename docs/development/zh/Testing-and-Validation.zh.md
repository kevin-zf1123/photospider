# 测试与验证

`cmake/KernelTests.cmake`、`cmake/PackageTests.cmake`、`cmake/BehaviorTests.cmake` 及可选 `cmake/BenchmarkTools.cmake` 定义可执行测试、依赖、标签、超时和跳过规则。注册不证明测试已经执行。

## 测试归属与生命周期

新增或重新迁移的永久场景只保留已确认缺陷的最小回归及既有包契约。必要的正向对照必须直接支撑缺陷复现。历史场景逐项追溯：文件改名、迁目录或适配 ABI 都不能证明历史缺陷；混合文件也不能证明每个场景都应保留。旧 API 编译失败或人为 mutation 后失败，单独均不足以确认缺陷。

一般功能覆盖、预防性负面场景、组合与性能对照可作临时验收。验证后删除其源码、数据、hook、fixture target 和 CMake/CTest 注册。普通 ABI 覆盖不能通过改名为 package contract 绕过政策。保留 `oracle/ops` 下的独立算子 oracle、数学参考、容差和既有参考数据，不为实现修改它们。

内核回归位于 `tests/unit/kernel` 和 `tests/integration/kernel`，不依赖 bundled ops 构建。既有包契约位于 `tests/package`。版本契约检查生成的 SameMinorVersion 行为，不安装包、不构建外部 consumer。内核开发完成前，不新增、恢复或运行安装消费测试。

内置算子数值检查、跨算子 workflow 和示例检查按需现场编写，验证完成后删除。规格验收场景不等于永久测试清单。已删除的整体结果、动态 field 和默认语义认证断言，不机械改写为同等规模的预防性覆盖。

Benchmark runner/oracle/report 类型属于默认关闭的 `Photospider::benchmark_tools`。`tests/tools` 的工具回归遵循相同的已确认缺陷政策；启用 target 不意味着保留所有 benchmark 场景。Kernel 保留原始计时、计数和资源观测。

## 配置与执行

复用已有配置目录，选择受影响目标与测试：

```sh
cmake --build build/kernel-dev --target <affected-targets> -j 8
ctest --test-dir build/kernel-dev -R '<focused-test-regex>' --output-on-failure
```

解释 CTest 结果前先构建实际测试目标，仅在需要时配置。完整 CTest、clean build、sanitizer 和平台矩阵不是默认步骤，须有用户明确授权，或任务直接以相应验证 artifact 为对象；reviewed 风险判断本身不构成授权。修改的 C++ 文件使用 ClangFormat 21 和 cpplint。

并发回归用事件或条件变量控制交错，不依赖猜测 sleep。只有确认保留回归不再依赖后，才删除孤立 helper、hook 和 DSO target。私有测试接缝不进入公共插件 ABI。

## C Result 表拒绝

保留的 C11 fixture 通过 `OperationRegistry::load_plugin` 复现真实 importer 缺陷。`test_result_repeated` 保留 valid-table 对照和历史缺失 resolver 的 case 6。其他表场景按自身确认历史筛选，fixture 数量不是兼容承诺。当前依赖见 `cmake/KernelTests.cmake`。

Module table 和状态在 DSO owner 前退役。生命周期断言须在 observer module 仍映射时观察真实销毁。仅 ABI-size 拒绝不证明此前的行为缺陷。

## 行为边界

测试断言可观察数值、覆盖、错误、取消、所有权和资源效果。Tile Need 与不可变共享证据定义依赖，恢复证据不恢复读取权限。缓存命中本身不是正确性 oracle。修复前后失败证据和最小输入/断言界定保留回归。

分别报告 CPU、mock、native 通过、native 失败和 native 跳过。设备或后端不可用时，硬件测试可使用 skip code 77；native 工作准入后，意外失败仍是失败。Mock 或数学 oracle 不证明原生 GPU 执行。只报告实际运行命令，明确未验证后端。

错误回归隔离相关条件；传播是契约时比较原始结构化状态。规定计费或生命周期事件可以断言精确次数；实现内部命中率或调度计时不作为永久行为保证。
