# GitHub Actions CI

Photospider 有意只保留两个 GitHub Actions workflow：

- `.github/workflows/ci.yml` 处理日常 push，包含一次 healthcheck、一次可复用构建和三个并行 CTest label 分片。
- `.github/workflows/build-ci-image.yml` 在 `main` 的 `Dockerfile.ci` 发生变更或维护者手工触发时发布 Linux CI 镜像。

仓库不再设置单独的 pull-request-target、sanitizer、scheduler-log、routing、evidence、provenance 或 aggregator workflow。本仓库按个人开发项目维护，因此 CI 围绕真正有用的构建与测试信号安排，而不是复刻 enterprise 审批或自授权证明机制。

## 日常 CI 流程

`.github/workflows/ci.yml` 在向 `main` 和 `CI/**` push 时运行。每个 job 都使用 `ghcr.io/<owner>/<repo>/photospider-ci:latest`、递归 checkout submodule，并且只拥有仓库内容与 package 的读取权限。Job 形成一条带并行测试叶子的依赖链：

```text
healthcheck -> build -> unit
                     -> integration
                     -> verification
```

### Healthcheck

在执行项目检查前，Healthcheck 只把 `$GITHUB_WORKSPACE` 加入 job container
用户的全局 Git `safe.directory` 列表，并验证已 checkout 的 `HEAD` 是 commit。
这只是用于跨 checkout 所有权边界保证容器内 Git 可用的设置，不是授权、
protected-path 或 provenance 证明；它绝不使用通配符。

随后 Healthcheck 有意只执行三项低成本检查：

1. 对 push 的 commit 运行 `git diff --check`。
2. 启用测试并关闭 ASan、TSan 与 fuzzer，执行一次 CMake configure。
3. 构建 `public_header_self_containment`。

它不会对变更路径分类、推断 docs-only 运行、比较 protected path、检查其他 ref、构建完整产品，也不会决定后续 job 是否存在。

### 一次缓存构建

Build job 使用 `actions/cache@v6` 恢复 `build/ci`。Cache key 包含：

- 显式 cache epoch；
- runner 操作系统与 build type；
- 对 `Dockerfile.ci`、所有 `CMakeLists.txt` 和 `cmake/**` 计算的一份 hash；
- 精确 Git commit SHA。

Restore prefix 在 SHA 前结束，因此一次 push 可以复用最近兼容的旧 build tree。每次运行仍会对恢复后的树执行 CMake configure，随后只调用一次 Ninja 完成主构建。Job 结束时，`actions/cache` 会保存最终完整构建树，其中包括 object file 与 Ninja dependency state，供下一次兼容 push 使用。
Workflow 还会打印 action 的 `cache-hit` output，因此重跑时可以区分 exact-key hit、prefix restore 与 complete miss，而不改变 cache 行为。

主构建结束后，job 会立即打包轻量 CTest runtime，随后运行 CTest 的 `build-smoke` label。这些测试覆盖嵌套 configure、install、package consumer、option-off 和 public header 构建契约。先打包可使临时嵌套 build tree 留在 build cache 中，而不会把本次调用新建的 tree 加入 runtime artifact。恢复的 cache 可能已经包含较早运行留下的固定 work root：`tests/image_artifact_codec_dependency_disabled` 与 `tests/optional_opencv_provider_disabled`；因此 packager 会精确排除这两个临时 root 及其所有后代，但不会从缓存 build tree 中删除它们。测试的 `RUN_SERIAL` 与 `TIMEOUT` 属性保存在 `CMakeLists.txt` 中，因此 workflow 不需要维护 smoke test 名称列表。只有完整 label 成功后才会上传 runtime archive；smoke 失败因此会阻断该上传和所有依赖的 test job。

Workflow 会在 `always()` 条件下单独尝试上传 build-smoke JUnit report，即使 CTest 失败也会执行。Artifact 名称为 `ctest-junit-build-smoke`，读取 `CI-results/build-smoke.junit.xml`，将存在的 report 保留七天；文件缺失时只告警而不会使 step 失败。JUnit report 绝不会打包进 `ctest-runtime`。

### 轻量 CTest runtime

`ci/scripts/package_ctest_runtime.sh` 会在本次 build-smoke label 运行前，从已经完成构建的 `build/ci` 创建唯一一份物理文件 `ctest-runtime.tar.gz`。归档会保留 CTest 运行所需的 runtime closure：

- 静态与动态库；
- operation、policy 与 scheduler plugin；
- 测试与产品 executable；
- `CTestTestfile.cmake` 与生成的 GoogleTest inventory；
- `CMakeCache.txt`、生成的 package configuration 和其他 runtime data。

归档排除所有 `.o` 与 `.obj`、所有 `CMakeFiles` tree、`.ninja_deps`、`.ninja_log`、既有 `Testing` 输出，以及上文精确命名的两个临时 nested-smoke root。这些增量构建输入与 smoke work input 只留在 build cache 中；`tests/` 的其余内容仍作为 CTest runtime 保留。Build-smoke 成功后，归档只上传一次，关闭 artifact 二次压缩，并由三个测试 job 共同下载。
Packager 在验证 closure 后会打印物理 archive byte count 与 tar entry count。这些值只是 cache/artifact 体积实验的 observation，不会放宽 required root 或 forbidden entry 检查。

每个测试 job 都把归档恢复到 producer 使用的相同路径 `build/ci`，随后运行一个精确的 primary label。CI 不会生成重复 runtime package，也不会上传完整 build-tree artifact。每次 label 调用结束后，一个 `always()` step 会单独尝试把 `CI-results/ctest/<label>.junit.xml` 上传为唯一的 `ctest-junit-<label>` artifact。存在的 report 会保留七天；report 缺失时只告警，不会使 job 失败。

## CTest label 与并行约束

CMake 负责测试选择。默认 push 路径中的每个测试都具有一个 primary label：

- `unit`：源码角色位于 `tests/unit/` 的持续维护 GoogleTest；
- `integration`：持续维护的 integration GoogleTest，以及直接 runtime/CLI 检查；
- `verification`：适合日常 CI 的确定性 safety harness；
- `build-smoke`：在 build job 中运行的嵌套构建、安装和 package 检查。

测试可以继续保留 `execution`、`security`、`kernel-concurrency` 或 `value-runtime` 等正交 label。仓库 discovery wrapper 会解析并验证 caller 的每组 property pair，把 source-role primary label 与这些正交 label 去重，并只向 `gtest_discover_tests` 传递一个标量 primary `LABELS` property。由于 upstream module 不能传递 list-valued property，生成的 `TEST_INCLUDE_FILES` script 会在 discovery 后使用各自的 `TEST_LIST`，一次性设置完整 merged list 与全部 caller test property；仓库既不依赖重复 property 的隐式合并，也不依赖 module 的 list-value transport。未知 discovery argument、未知 property、奇数长度 property list 与重复的非 label property 会在 configure 阶段失败。共享可变 harness 或不能重叠运行的测试在 CMake 中声明 `RESOURCE_LOCK` 或 `RUN_SERIAL`，需要边界的测试在 CMake 中声明 `TIMEOUT`。因此 workflow 只包含 `--label-regex '^<primary-label>$'`，不会编码冗长的包含或排除正则。

重型 benchmark、fuzz target 与 sanitizer build 是 opt-in 开发者工具。日常 push 会配置 `USE_ASAN=OFF`、`USE_TSAN=OFF` 和 `PHOTOSPIDER_BUILD_FUZZERS=OFF`；默认 job 不运行这些工作负载。

## CI 镜像 workflow

`Dockerfile.ci` 定义 Linux toolchain，并包含 Ninja 和项目构建依赖。`.github/workflows/build-ci-image.yml` 只在 `main` 的 `Dockerfile.ci` 变更或手工 dispatch 时运行。它向 `ghcr.io/<owner>/<repo>/photospider-ci` 发布 `latest`、commit tag 和可选 manual tag。

日常 CI workflow 消费这份已发布镜像。Dockerfile 变更必须先成功落地并发布，后续 push 才能依赖新 toolchain；日常 CI 本身不会构建或比较镜像。

两个 workflow 都使用持续维护的 Node 24 action major：`actions/checkout@v7`、`actions/cache@v6`、`actions/upload-artifact@v7`、`actions/download-artifact@v8`、`docker/login-action@v4`、`docker/metadata-action@v6` 与 `docker/build-push-action@v7`。这些 major 要求 Actions Runner 2.327.1 或更新版本；当前 workflow 使用 GitHub-hosted runner，而不是仓库自有的 self-hosted runner。

## 本地检查

本地验证应使用 native build；不要仅为复刻 GitHub Actions 而在 macOS 上模拟 Linux 镜像：

```bash
cmake -S . -B build/ci -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DBUILD_TESTING=ON \
  -DUSE_ASAN=OFF \
  -DUSE_TSAN=OFF \
  -DPHOTOSPIDER_BUILD_FUZZERS=OFF
cmake --build build/ci --parallel 2
bash ci/scripts/package_ctest_runtime.sh \
  build/ci CI-results/ctest-runtime.tar.gz
tar -tzf CI-results/ctest-runtime.tar.gz
ctest --test-dir build/ci --output-on-failure --parallel 2
```

聚焦本地运行使用与 CI 相同的 primary label：

```bash
ctest --test-dir build/ci --output-on-failure -L '^unit$'
ctest --test-dir build/ci --output-on-failure -L '^integration$'
ctest --test-dir build/ci --output-on-failure -L '^verification$'
ctest --test-dir build/ci --output-on-failure -L '^build-smoke$'
```

持续维护的 `graph_cli_script_test.sh`、`propagation_script_test.sh`、`plugin_load_test.sh`、`execution_repeat_test.sh` 和 `sanitizer_test.sh` 仍可用于显式本地 product-boundary 或 sanitizer 检查。它们不是额外的 GitHub workflow，也不属于日常 push 路径。
