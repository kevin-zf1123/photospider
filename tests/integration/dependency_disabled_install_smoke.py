#!/usr/bin/env python3
"""Exercise the installed OpenCV/YAML-disabled Host product."""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path


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
        "BUILD_TESTING": "OFF",
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
                "#include <filesystem>",
                "#include <fstream>",
                "#include <memory>",
                "#include <string>",
                "",
                "#include <photospider/core/graph_error.hpp>",
                "#include <photospider/core/image_buffer.hpp>",
                "#include <photospider/core/result_types.hpp>",
                "#include <photospider/host/host.hpp>",
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
    """@brief Build, install, and consume the dependency-disabled profile.

    @return Zero only when the kernel aggregate and Host product build without
      OpenCV/YAML discovery, installation omits their public/export surfaces,
      optional/required component semantics are correct, and the real consumer
      exercises neutral image and Host behavior.
    @throws OSError For filesystem or process-start failures.
    @throws subprocess.CalledProcessError For required command failures.
    @throws ValueError If transient paths overlap protected paths.
    @throws RuntimeError If any build, export, component, or runtime invariant
      contradicts the dependency-disabled profile.
    @note A validated ``--producer-build`` may be reused without configuration
      or compilation. Installation and consumer artifacts always remain under
      ``work`` and are removed before return.
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
                    "-DBUILD_TESTING=OFF",
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
                    "--config",
                    args.config,
                    "--parallel",
                    "4",
                ],
                repo,
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
