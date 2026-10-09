# 测试与验证

本文说明已注册的行为测试和安装包 gate。`CMakeLists.txt`、`cmake/BehaviorTests.cmake` 与 `tests/consumer/CMakeLists.txt` 中的声明共同定义每个测试的可执行文件、fixture、依赖、命令、标签、timeout 和 skip code。

## 配置与运行

配置前先将固定版本 SLEEF 3.9.0 源码放到 `third_party/sleef/`；CMake 不会自动下载。常规本地构建命令如下：

```sh
cmake -S . -B build/check -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=ON
cmake --build build/check -j8
ctest --test-dir build/check --output-on-failure
```

`photospider_tests` 构建全部已注册测试可执行文件。CTest 标签可以选择行为族，例如 `ctest --test-dir build/check -L 'unit|execution' --output-on-failure`；`ctest --test-dir build/check --print-labels` 可查看标签。Unit 测试 timeout 为 120 秒，integration 为 300 秒，大型 numeric 或 image 测试为 600 秒。只有顶层 `test_installed_consumer` 的 timeout 为 1800 秒。Nested `installed_*` 测试按所属类型使用常规上限：unit 为 120 秒，integration 为 300 秒，大型 numeric 或 image 测试为 600 秒。Sanitizer build 将各自 timeout 延长三倍。

## 安装包 consumer

`test_installed_consumer` 将当前构建的 package 安装到隔离 prefix，使用 `find_package(Photospider 0.33 CONFIG REQUIRED COMPONENTS kernel ops_headers operation_sdk data_provider_sdk)` 配置 `tests/consumer`，再构建并运行默认 package gate。Gate 只使用安装后的 public headers 和导出 targets。每个 consumer 可执行文件都链接 `Photospider::kernel` 和 `Photospider::ops_headers`，因此安装后的 `photospider/ops/` 辅助头文件会与内核头文件一起受到检查。仓库外使用这些算子辅助的项目采用同样的链接方式。Consumer 注册声明同时派生测试命令和构建依赖，包括 fixture DSO。需要 private symbols 的测试链接非安装的 static test library，该库使用正常 kernel objects 构建；fault-injection 变体只替换包含对应 test seam 的编译单元。该库同时编译私有 execution test hooks，并发测试借此等待真实事件，不依赖计时。Phase work gate 让 callback 线程停在 executor poll 内选定的一次正值 work 计费处。Shared-join event 在 waiter 取得 shared producer 的 lease 后触发。Operation fixture DSO 导出 operation ABI 之外的私有 arm、wait 和 release 符号，用于挂起 native callback 直到测试放行；放行后 callback 调用真实的 cancellation service。本地注册的 `test_computed_scalar_sharing`（`test_computed_scalar --sharing-only`）使用 `PHOTOSPIDER_LOCAL_EXECUTION_SYNC_TESTS` 构建，覆盖共享 computed scalar 的某一调用方取消。安装测试 `installed_computed_scalar` 只针对安装后的 public package 构建同一源码，不含这些 hooks，并拒绝任何命令行参数。

Consumer 项目提供两个明确 target：

```sh
cmake -S tests/consumer -B build/consumer-build \
  -DCMAKE_PREFIX_PATH=/path/to/photospider-prefix
cmake --build build/consumer-build --target run_photospider_consumer
cmake --build build/consumer-build --target run_photospider_consumer_all
```

`run_photospider_consumer` 运行 12 项核心 package gate。`run_photospider_consumer_all` 构建已注册的 installed targets 并运行全部 39 项 installed tests，其中包含可选硬件检查。设备不可用时，测试使用已注册的 skip code 77。Skip 表示没有执行对应硬件路径。

顶层 installed gate 在配置 kernel build 后通过 CTest 运行。其 nested configure 会收到当前 generator、platform、toolset、configuration 和 sanitizer mode。匹配的 sanitizer instrumentation 会进入 consumer 的 C/C++ 编译和链接步骤，不会进入安装 package 导出文件。仓内及 installed consumer 的 `MODULE` fixture 使用相同的 sanitizer instrumentation；C11 operation fixture 仍以 C 编译。macOS ASAN 构建会在每个 module 中加入测试专用 C++ atexit owner，在 `dlclose` 执行模块退休期间、image 仍映射时注销 compiler-rt image globals。实现见[module instrumentation helper](../../../cmake/BehaviorTests.cmake)和[retirement owner](../../../tests/support/asan_module_retirement.cpp)。


## C Result table 拒绝测试

共享的[`photospider_add_result_table_fixture` helper](../../../cmake/BehaviorTests.cmake)将坏 Result table fixture 编译为 C11 `MODULE` 库。四个带 selector 的 DSO 覆盖 34 个非法表案例：operation 14 个、joint 10 个、repeated input 6 个、RequestRecord 4 个。四个基础 fixture 分别是[`result_operation_fixture.c`](../../../tests/fixtures/result_operation_fixture.c)、[`result_joint_fixture.c`](../../../tests/fixtures/result_joint_fixture.c)、[`result_repeated_fixture.c`](../../../tests/fixtures/result_repeated_fixture.c)和[`result_request_record_fixture.c`](../../../tests/fixtures/result_request_record_fixture.c)，共用[bad-table selector support](../../../tests/fixtures/bad_result_table_support.h)。[测试 helper](../../../tests/support/bad_result_table_fixture.hpp)选择一个案例并调用真实的 `OperationRegistry::load_plugin`；每个案例检查预期的 `Status.code`、空 registry key 列表和准确的 plugin destroy 数量。关闭 observer handle 前先销毁 registry 和 candidate，以便 DSO 仍映射时验证退休行为。成功 fixture module 与单独的 C++ throwing joint fixture 仍各自独立。

Root integration 测试为[`test_result_plugin`](../../../tests/integration/test_result_plugin.cpp)、[`test_result_c_joint`](../../../tests/integration/test_result_c_joint.cpp)、[`test_result_repeated`](../../../tests/integration/test_result_repeated.cpp)和[`test_result_request_record`](../../../tests/integration/test_result_request_record.cpp)。Fixture 构建依赖由[root CMake 注册声明](../../../CMakeLists.txt)提供。聚焦运行命令如下：

```sh
cmake --build build/check --target \
  test_result_plugin test_result_c_joint test_result_repeated \
  test_result_request_record -j8
ctest --test-dir build/check \
  -R '^(test_result_plugin|test_result_c_joint|test_result_repeated|test_result_request_record)$' \
  --output-on-failure
```

Installed consumers 在[`tests/consumer/CMakeLists.txt`](../../../tests/consumer/CMakeLists.txt)中复用相同 integration sources 和 bad-table helper，名称为 `installed_unified_result_c11`、`installed_result_c_joint`、`installed_result_repeated` 和 `installed_result_request_record`。它们是 installed suite 中的普通必需测试，不是硬件 skip。按上文配置好 `tests/consumer` 后，运行：

```sh
cmake --build build/consumer-build --target \
  photospider_result_c_consumer photospider_result_c_joint_consumer \
  photospider_repeated_result_consumer photospider_request_record_consumer
ctest --test-dir build/consumer-build \
  -R '^(installed_unified_result_c11|installed_result_c_joint|installed_result_repeated|installed_result_request_record)$' \
  --output-on-failure
```

## 行为边界

已注册测试覆盖 compiler 校验与规划、Result operations 和 C ABI 加载、执行与取消、依赖关系与共享工作、资源限制、数值与图像行为以及原生 backend dispatch。Installed consumers 覆盖 package 导出、public C/C++ headers、expression metadata resolver、Result C/C++ workflow、repeated-input 契约、binding 与 demand、cache、取消和 backend admission。

硬件专属测试以 skip return code 77 注册。确定性 CPU 或 fault-injection 测试可以覆盖相关软件契约，但不能证明原生设备执行。报告时应分别说明 CPU/mock 覆盖和实际硬件执行。

测试检查可观察的数值、错误类别、资源限制、取消、发布和所有权行为。Block-cache 命中数等缓存内部计数属于实现细节；性能基准与行为测试分开处理。
