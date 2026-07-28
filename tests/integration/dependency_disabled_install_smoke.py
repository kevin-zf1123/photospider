#!/usr/bin/env python3
"""Exercise the OpenCV/YAML-disabled dense runtime and installed Host product."""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

from cmake_build_smoke_support import (
    producer_osx_architecture_arguments,
)


def run(command: list[str], cwd: Path) -> None:
    """@brief Run one required smoke command with inherited output.

    @param command Executable and arguments passed without a shell.
    @param cwd Working directory for the child process.
    @return None.
    @throws OSError If the command cannot start.
    @throws subprocess.CalledProcessError If the command exits nonzero.
    @note Inherited streams remain visible in CTest and CI artifacts.
    """

    print("$ " + " ".join(command), flush=True)
    subprocess.run(command, cwd=cwd, check=True)


def run_expect_failure(
    command: list[str], cwd: Path, expected_diagnostic: str
) -> None:
    """@brief Require one configure command to fail with a stable diagnostic.

    @param command Executable and arguments passed without a shell.
    @param cwd Working directory for the child process.
    @param expected_diagnostic Text that must occur in combined child output.
    @return None after a nonzero child status.
    @throws OSError If the command cannot start.
    @throws RuntimeError If the command succeeds or omits the diagnostic.
    @note Combined child output remains visible in CTest and CI artifacts.
    """

    print("$ " + " ".join(command), flush=True)
    completed = subprocess.run(
        command,
        cwd=cwd,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    print(completed.stdout, end="", flush=True)
    if completed.returncode == 0:
        raise RuntimeError("expected configure command to fail")
    normalized_output = " ".join(completed.stdout.split())
    normalized_diagnostic = " ".join(expected_diagnostic.split())
    if normalized_diagnostic not in normalized_output:
        raise RuntimeError(
            "failed configure omitted expected diagnostic: "
            f"{expected_diagnostic}"
        )


def remove_tree(path: Path, repo: Path) -> None:
    """@brief Remove one validated transient build/install tree.

    @param path Work directory to remove when present.
    @param repo Repository root that must never be removed.
    @return None.
    @throws ValueError If path is the repository or one of its ancestors.
    @throws OSError If recursive removal fails.
    @note Every descendant under path is owned by this smoke.
    """

    if path == repo or path in repo.parents:
        raise ValueError(f"refusing destructive work path: {path}")
    if path.exists() or path.is_symlink():
        shutil.rmtree(path)


def cmake_cache_values(build: Path) -> dict[str, str]:
    """@brief Read exact key/value assignments from one CMake cache.

    @param build Existing producer build directory containing CMakeCache.txt.
    @return Mapping from cache keys to their final serialized values.
    @throws OSError If the cache cannot be read.
    @throws RuntimeError If the producer has no regular cache file.
    @note Comments and malformed lines are ignored; later assignments win.
    """

    cache_path = build / "CMakeCache.txt"
    if not cache_path.is_file():
        raise RuntimeError(f"reusable producer has no CMake cache: {cache_path}")
    values: dict[str, str] = {}
    for line in cache_path.read_text(encoding="utf-8").splitlines():
        if not line or line.startswith(("#", "//")) or "=" not in line:
            continue
        assignment, value = line.split("=", 1)
        if ":" not in assignment:
            continue
        key, _cache_type = assignment.split(":", 1)
        if key:
            values[key] = value
    return values


def validate_reusable_producer(repo: Path, build: Path, config: str) -> None:
    """@brief Validate an external dependency-disabled producer.

    @param repo Resolved Photospider source repository expected by the cache.
    @param build Resolved reusable producer build directory.
    @param config Requested single- or multi-config build configuration.
    @return None after identity and profile checks succeed.
    @throws OSError If cached paths cannot be read or resolved.
    @throws RuntimeError If identity, configuration, or capability state differs.
    @note Validation is fail-closed and never mutates the producer.
    """

    cache = cmake_cache_values(build)

    def require(key: str) -> str:
        """@brief Return one required serialized cache value.

        @param key Exact CMake cache key.
        @return Serialized value, including an intentional empty value.
        @throws RuntimeError If key is absent.
        @note Callers own profile-specific interpretation.
        """

        if key not in cache:
            raise RuntimeError(f"reusable producer cache is missing {key}")
        return cache[key]

    cached_source = Path(require("CMAKE_HOME_DIRECTORY")).resolve()
    cached_build = Path(require("CMAKE_CACHEFILE_DIR")).resolve()
    if cached_source != repo or cached_build != build:
        raise RuntimeError(
            "reusable producer identity mismatch: "
            f"source={cached_source}, build={cached_build}"
        )
    expected_values = {
        "BUILD_TESTING": "ON",
        "PHOTOSPIDER_BUILD_IPC": "OFF",
        "PHOTOSPIDER_ENABLE_OPENCV": "OFF",
        "PHOTOSPIDER_ENABLE_YAML": "OFF",
        "PHOTOSPIDER_BUILD_GRAPH_CLI": "OFF",
        "PHOTOSPIDER_BUILD_OPENCV_OPERATION_PROVIDER": "OFF",
        "PHOTOSPIDER_BUILD_OPENCV_OPERATION_PLUGINS": "OFF",
        "CMAKE_DISABLE_FIND_PACKAGE_OpenCV": "ON",
        "CMAKE_DISABLE_FIND_PACKAGE_yaml-cpp": "ON",
    }
    for key, expected in expected_values.items():
        actual = require(key)
        if actual != expected:
            raise RuntimeError(
                f"reusable producer requires {key}={expected}, got {actual}"
            )

    configuration_types = cache.get("CMAKE_CONFIGURATION_TYPES", "")
    if configuration_types:
        if config not in configuration_types.split(";"):
            raise RuntimeError(
                f"requested configuration {config} is unavailable: "
                f"{configuration_types}"
            )
    elif require("CMAKE_BUILD_TYPE") != config:
        raise RuntimeError(
            f"reusable producer build type is not requested {config}"
        )


def configured_test_executable(
    build: Path, config: str, target_name: str
) -> Path:
    """@brief Resolve one built dependency-neutral integration binary.

    @param build Configured dependency-disabled producer build directory.
    @param config Requested single- or multi-config build configuration.
    @param target_name Exact executable target basename without platform suffix.
    @return Expected executable path for the cached generator mode.
    @throws OSError If the producer cache cannot be read.
    @throws RuntimeError If configuration metadata is missing or contradicts
      the requested configuration.
    @note The caller executes the returned path directly, so a missing or
      non-runnable target fails through the ordinary process-start boundary.
    """

    cache = cmake_cache_values(build)
    configuration_types = cache.get("CMAKE_CONFIGURATION_TYPES", "")
    if configuration_types:
        available = [
            candidate.strip()
            for candidate in configuration_types.split(";")
            if candidate.strip()
        ]
        if config not in available:
            raise RuntimeError(
                "dependency-disabled test configuration mismatch: "
                f"requested {config}, available {available}"
            )
        output_directory = build / "tests" / config
    else:
        build_type = cache.get("CMAKE_BUILD_TYPE")
        if build_type != config:
            raise RuntimeError(
                "dependency-disabled test build type mismatch: "
                f"requested {config}, got {build_type}"
            )
        output_directory = build / "tests"
    executable_suffix = ".exe" if sys.platform == "win32" else ""
    return output_directory / (target_name + executable_suffix)


def write_component_probe(source: Path, *, required: bool) -> None:
    """@brief Write one unavailable operation_opencv component probe.

    @param source Source directory created for the probe.
    @param required Whether discovery must require the unavailable component.
    @return None.
    @throws OSError If source files cannot be written.
    @note Optional discovery must succeed without creating an imported target;
      required discovery is expected to fail during configure.
    """

    source.mkdir(parents=True)
    if required:
        body = [
            "cmake_minimum_required(VERSION 3.16)",
            "project(required_opencv_component LANGUAGES CXX)",
            "find_package(Photospider CONFIG REQUIRED",
            "  COMPONENTS operation_opencv)",
            "",
        ]
    else:
        body = [
            "cmake_minimum_required(VERSION 3.16)",
            "project(optional_opencv_component LANGUAGES CXX)",
            "find_package(Photospider CONFIG",
            "  OPTIONAL_COMPONENTS operation_opencv)",
            "if(NOT Photospider_FOUND)",
            '  message(FATAL_ERROR "optional component lookup failed package")',
            "endif()",
            "if(Photospider_operation_opencv_FOUND OR",
            "   TARGET Photospider::operation_opencv)",
            '  message(FATAL_ERROR "disabled OpenCV component was advertised")',
            "endif()",
            "",
        ]
    (source / "CMakeLists.txt").write_text("\n".join(body), encoding="utf-8")


def write_consumer(source: Path) -> None:
    """@brief Write the installed Host consumer and its CMake project.

    @param source Source directory created for the consumer.
    @return None.
    @throws OSError If source files cannot be written.
    @note The executable verifies neutral allocation, empty-session lifecycle,
      and explicit persistence failure without any parser or image-library API.
    """

    source.mkdir(parents=True)
    (source / "CMakeLists.txt").write_text(
        "\n".join(
            [
                "cmake_minimum_required(VERSION 3.16)",
                "project(dependency_disabled_consumer LANGUAGES CXX)",
                "find_package(Photospider CONFIG REQUIRED COMPONENTS embedded)",
                "add_executable(dependency_disabled_consumer main.cpp)",
                "target_link_libraries(dependency_disabled_consumer",
                "  PRIVATE Photospider::photospider)",
                "",
            ]
        ),
        encoding="utf-8",
    )
    (source / "main.cpp").write_text(
        "\n".join(
            [
                "#include <algorithm>",
                "#include <cstddef>",
                "#include <filesystem>",
                "#include <fstream>",
                "#include <memory>",
                "#include <string>",
                "#include <utility>",
                "#include <vector>",
                "",
                "#include <photospider/core/graph_error.hpp>",
                "#include <photospider/core/image_buffer.hpp>",
                "#include <photospider/core/result_types.hpp>",
                "#include <photospider/data/image_view.hpp>",
                "#include <photospider/data/region.hpp>",
                "#include <photospider/data/value.hpp>",
                "#include <photospider/host/host.hpp>",
                "#include <photospider/memory/buffer_handle.hpp>",
                "",
                "int main(int argc, char** argv) {",
                "  if (argc != 2) return 10;",
                "  const std::filesystem::path root(argv[1]);",
                "  std::filesystem::create_directories(root);",
                "",
                "  ps::ImageBuffer image = ps::make_aligned_cpu_image_buffer(",
                "      3, 2, 4, ps::DataType::UINT8);",
                "  ps::validate_image_buffer(image);",
                "  if (!image.data || image.width != 3 || image.height != 2) {",
                "    return 11;",
                "  }",
                "",
                "  const ps::RegionSet region = ps::RegionSet::from_image_rect(",
                "      {ps::image_region_domain(), 0, 3, 0, 2});",
                "  const auto overlap = ps::intersect_regions(",
                "      region, ps::RegionSet::from_image_rect(",
                "                  {ps::image_region_domain(), 1, 3, 1, 2}));",
                "  if (overlap.status() != ps::RegionOperationStatus::Exact ||",
                "      !overlap.region().has_value() ||",
                "      ps::region_contains(region, *overlap.region()) !=",
                "          ps::RegionContainmentStatus::Contains) {",
                "    return 17;",
                "  }",
                "",
                "  ps::DenseTensorDescriptor descriptor{",
                "      {2U, 3U, 1U}, ps::ElementSemantics::UnsignedInteger,",
                "      ps::StorageEncoding{8U}};",
                "  ps::ImageFacet facet;",
                "  facet.x_axis = 1U;",
                "  facet.y_axis = 0U;",
                "  facet.channel_axis = 2U;",
                "  ps::StridedLayout layout{{3, 1, 1}};",
                "  std::vector<std::byte> storage{",
                "      std::byte{1}, std::byte{2}, std::byte{3},",
                "      std::byte{4}, std::byte{5}, std::byte{6}};",
                "  auto builder = ps::ValueBuilder::allocate_cpu_dense_tensor(",
                "      std::move(descriptor), facet, std::move(layout),",
                "      storage.size());",
                "  {",
                "    auto write = builder.acquire_write();",
                "    std::copy(storage.begin(), storage.end(), write.data());",
                "  }",
                "  const ps::Value value = builder.seal();",
                "  const auto read = value.buffer_handle().acquire_read();",
                "  const ps::ImageView view(value);",
                "  if (!read.valid() || read.size() != storage.size() ||",
                "      read.allocation_identity() !=",
                "          value.allocation_identity() ||",
                "      !value.revision_id().valid() || view.width() != 3U ||",
                "      view.height() != 2U ||",
                "      std::to_integer<unsigned int>(",
                "          *view.channel_data(2U, 1U, 0U)) != 6U) {",
                "    return 16;",
                "  }",
                "",
                "  auto host = ps::create_embedded_host();",
                "  if (!host) return 12;",
                "  ps::GraphLoadRequest empty_request;",
                '  empty_request.session.value = "empty";',
                "  empty_request.root_dir = root.string();",
                "  auto loaded = host->load_graph(empty_request);",
                "  if (!loaded.status.ok) return 13;",
                "  auto closed = host->close_graph(loaded.value);",
                "  if (!closed.status.ok) return 14;",
                "",
                '  const auto document = root / "disabled.yaml";',
                "  std::ofstream(document) << \"nodes: []\\n\";",
                "  ps::GraphLoadRequest explicit_request;",
                '  explicit_request.session.value = "explicit";',
                "  explicit_request.root_dir = root.string();",
                "  explicit_request.yaml_path = document.string();",
                "  const auto rejected = host->load_graph(explicit_request);",
                "  const auto code = ps::checked_graph_error_code(",
                "      rejected.status);",
                "  if (rejected.status.ok || !code ||",
                "      *code != ps::GraphErrc::Io) {",
                "    return 15;",
                "  }",
                "  return 0;",
                "}",
                "",
            ]
        ),
        encoding="utf-8",
    )


def main() -> int:
    """@brief Execute, install, and consume the dependency-disabled profile.

    @return Zero only when the kernel aggregate and Host product build without
      OpenCV/YAML discovery, the core dense operation executes through its
      production registry/executor chain, installation omits optional
      public/export surfaces, component semantics are correct, and the real
      consumer exercises neutral image and Host behavior.
    @throws OSError For filesystem or process-start failures.
    @throws subprocess.CalledProcessError For required command failures.
    @throws ValueError If transient paths overlap protected paths.
    @throws RuntimeError If any build, export, component, or runtime invariant
      contradicts the dependency-disabled profile.
    @note A validated, already-built ``--producer-build`` may be reused without
      configuration or compilation. Its dependency-neutral dense and
      cross-DSO identity integration binaries are still executed. Installation
      and consumer artifacts always remain under ``work`` and are removed
      before return. On Darwin, every child configure inherits the selected
      producer's meaningful ``CMAKE_OSX_ARCHITECTURES`` value as one argv
      element; other platforms receive no macOS-specific option.
    """

    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", required=True)
    parser.add_argument("--work", required=True)
    parser.add_argument("--cmake-executable", default="cmake")
    parser.add_argument("--config", default="RelWithDebInfo")
    parser.add_argument("--producer-build", default="")
    args = parser.parse_args()

    repo = Path(args.repo).resolve()
    work = Path(args.work).resolve()
    producer_build = (
        Path(args.producer_build).resolve() if args.producer_build else None
    )
    if producer_build is not None:
        if (
            producer_build == work
            or producer_build in work.parents
            or work in producer_build.parents
        ):
            raise ValueError(
                "reusable producer and transient work paths overlap: "
                f"{producer_build}, {work}"
            )
        validate_reusable_producer(repo, producer_build, args.config)

    remove_tree(work, repo)
    build = producer_build if producer_build is not None else work / "build"
    prefix = work / "install"
    optional_source = work / "optional-opencv"
    optional_build = work / "optional-opencv-build"
    required_source = work / "required-opencv"
    required_build = work / "required-opencv-build"
    invalid_provider_build = work / "invalid-provider-build"
    invalid_plugins_build = work / "invalid-plugins-build"
    invalid_cli_build = work / "invalid-cli-build"
    consumer_source = work / "consumer"
    consumer_build = work / "consumer-build"
    runtime_root = work / "runtime"
    try:
        if producer_build is None:
            run(
                [
                    args.cmake_executable,
                    "-S",
                    str(repo),
                    "-B",
                    str(build),
                    "-DBUILD_TESTING=ON",
                    "-DPHOTOSPIDER_BUILD_IPC=OFF",
                    "-DPHOTOSPIDER_ENABLE_OPENCV=OFF",
                    "-DPHOTOSPIDER_ENABLE_YAML=OFF",
                    "-DCMAKE_DISABLE_FIND_PACKAGE_OpenCV=ON",
                    "-DCMAKE_DISABLE_FIND_PACKAGE_yaml-cpp=ON",
                    f"-DCMAKE_BUILD_TYPE={args.config}",
                ],
                repo,
            )
            run(
                [
                    args.cmake_executable,
                    "--build",
                    str(build),
                    "--target",
                    "photospider_kernel",
                    "photospider",
                    "test_cpu_dense_tensor_image_operation",
                    "test_value_identity_across_dsos",
                    "--config",
                    args.config,
                    "--parallel",
                    "4",
                ],
                repo,
            )

        dense_test_executable = configured_test_executable(
            build, args.config, "test_cpu_dense_tensor_image_operation"
        )
        identity_test_executable = configured_test_executable(
            build, args.config, "test_value_identity_across_dsos"
        )
        run([str(dense_test_executable)], repo)
        run([str(identity_test_executable)], repo)

        child_architecture_arguments = (
            producer_osx_architecture_arguments(build)
        )
        run(
            [
                args.cmake_executable,
                "--install",
                str(build),
                "--prefix",
                str(prefix),
                "--config",
                args.config,
            ],
            repo,
        )

        forbidden = [
            prefix / "include" / "photospider" / "plugin" / "opencv_adapter.hpp",
            *prefix.rglob("PhotospiderOpenCVTargets*.cmake"),
        ]
        existing_forbidden = [path for path in forbidden if path.exists()]
        if existing_forbidden:
            raise RuntimeError(
                "dependency-disabled install leaked OpenCV artifacts: "
                f"{existing_forbidden}"
            )
        embedded_exports = list(prefix.rglob("PhotospiderEmbeddedTargets*.cmake"))
        exported_text = "\n".join(
            path.read_text(encoding="utf-8") for path in embedded_exports
        )
        leaked_tokens = [
            token
            for token in (
                "Photospider::operation_opencv",
                "opencv_core",
                "opencv_imgproc",
                "opencv_imgcodecs",
                "yaml-cpp::yaml-cpp",
            )
            if token in exported_text
        ]
        if leaked_tokens:
            raise RuntimeError(
                f"dependency-disabled embedded export leaked: {leaked_tokens}"
            )

        write_component_probe(optional_source, required=False)
        run(
            [
                args.cmake_executable,
                "-S",
                str(optional_source),
                "-B",
                str(optional_build),
                f"-DCMAKE_PREFIX_PATH={prefix}",
                "-DCMAKE_DISABLE_FIND_PACKAGE_OpenCV=ON",
                "-DCMAKE_DISABLE_FIND_PACKAGE_yaml-cpp=ON",
                *child_architecture_arguments,
            ],
            repo,
        )

        write_component_probe(required_source, required=True)
        run_expect_failure(
            [
                args.cmake_executable,
                "-S",
                str(required_source),
                "-B",
                str(required_build),
                f"-DCMAKE_PREFIX_PATH={prefix}",
                "-DCMAKE_DISABLE_FIND_PACKAGE_OpenCV=ON",
                "-DCMAKE_DISABLE_FIND_PACKAGE_yaml-cpp=ON",
                *child_architecture_arguments,
            ],
            repo,
            "Photospider was built without the operation_opencv component",
        )

        common_disabled_configuration = [
            args.cmake_executable,
            "-S",
            str(repo),
            "-DBUILD_TESTING=OFF",
            "-DPHOTOSPIDER_BUILD_IPC=OFF",
            "-DPHOTOSPIDER_ENABLE_OPENCV=OFF",
            "-DPHOTOSPIDER_ENABLE_YAML=OFF",
            "-DCMAKE_DISABLE_FIND_PACKAGE_OpenCV=ON",
            "-DCMAKE_DISABLE_FIND_PACKAGE_yaml-cpp=ON",
            *child_architecture_arguments,
        ]
        run_expect_failure(
            [
                *common_disabled_configuration,
                "-B",
                str(invalid_provider_build),
                "-DPHOTOSPIDER_BUILD_OPENCV_OPERATION_PROVIDER=ON",
            ],
            repo,
            "PHOTOSPIDER_BUILD_OPENCV_OPERATION_PROVIDER=ON requires "
            "PHOTOSPIDER_ENABLE_OPENCV=ON",
        )
        run_expect_failure(
            [
                *common_disabled_configuration,
                "-B",
                str(invalid_plugins_build),
                "-DPHOTOSPIDER_BUILD_OPENCV_OPERATION_PLUGINS=ON",
            ],
            repo,
            "PHOTOSPIDER_BUILD_OPENCV_OPERATION_PLUGINS=ON requires "
            "PHOTOSPIDER_ENABLE_OPENCV=ON",
        )
        run_expect_failure(
            [
                *common_disabled_configuration,
                "-B",
                str(invalid_cli_build),
                "-DPHOTOSPIDER_BUILD_GRAPH_CLI=ON",
            ],
            repo,
            "PHOTOSPIDER_BUILD_GRAPH_CLI=ON requires "
            "PHOTOSPIDER_ENABLE_OPENCV=ON and PHOTOSPIDER_ENABLE_YAML=ON",
        )

        write_consumer(consumer_source)
        run(
            [
                args.cmake_executable,
                "-S",
                str(consumer_source),
                "-B",
                str(consumer_build),
                f"-DCMAKE_PREFIX_PATH={prefix}",
                "-DCMAKE_DISABLE_FIND_PACKAGE_OpenCV=ON",
                "-DCMAKE_DISABLE_FIND_PACKAGE_yaml-cpp=ON",
                *child_architecture_arguments,
            ],
            repo,
        )
        run(
            [
                args.cmake_executable,
                "--build",
                str(consumer_build),
                "--config",
                args.config,
                "--parallel",
                "4",
            ],
            repo,
        )
        executable_candidates = [
            path
            for name in (
                "dependency_disabled_consumer",
                "dependency_disabled_consumer.exe",
            )
            for path in consumer_build.rglob(name)
            if "CMakeFiles" not in path.parts and path.is_file()
        ]
        if not executable_candidates:
            raise RuntimeError("dependency-disabled consumer was not found")
        run([str(executable_candidates[0]), str(runtime_root)], work)
        print("Dependency-disabled install smoke: PASS", flush=True)
        return 0
    finally:
        remove_tree(work, repo)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:  # noqa: BLE001 - CTest needs one diagnostic.
        print(
            f"Dependency-disabled install smoke: FAIL: {error}",
            file=sys.stderr,
        )
        raise
