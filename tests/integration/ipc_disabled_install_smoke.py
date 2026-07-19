#!/usr/bin/env python3
"""Verify an IPC-disabled install advertises only the embedded product."""

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
    @note Inherited streams remain available to CTest and CI artifacts.
    """

    print("$ " + " ".join(command), flush=True)
    subprocess.run(command, cwd=cwd, check=True)


def run_expect_failure(command: list[str], cwd: Path) -> None:
    """@brief Require one product configuration command to fail.

    @param command Executable and arguments passed without a shell.
    @param cwd Working directory for the child process.
    @return None after a nonzero child status.
    @throws OSError If the command cannot start.
    @throws RuntimeError If the child unexpectedly succeeds.
    @note Inherited streams preserve the package component diagnostic in CTest.
    """

    print("$ " + " ".join(command), flush=True)
    completed = subprocess.run(command, cwd=cwd, check=False)
    if completed.returncode == 0:
        raise RuntimeError("expected package component configure to fail")


def remove_tree(path: Path, repo: Path) -> None:
    """@brief Remove one validated transient build/install tree.

    @param path Work directory to remove when present.
    @param repo Repository root that must never be removed.
    @return None.
    @throws ValueError If path is the repository or one of its ancestors.
    @throws OSError If recursive removal fails.
    @note The caller owns every descendant under path.
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
    @throws RuntimeError If the requested build has no regular cache file.
    @note Comments and malformed lines are ignored; later duplicate keys win,
      matching CMake's effective final assignment.
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
    """@brief Validate an external IPC-disabled producer without mutating it.

    @param repo Resolved Photospider source repository expected by the cache.
    @param build Resolved reusable producer build directory.
    @param config Requested single- or multi-config build configuration.
    @return None after every cache identity and profile check succeeds.
    @throws OSError If the cache cannot be read or a cached path cannot resolve.
    @throws RuntimeError If source/build identity, test/IPC state, or requested
      configuration does not match the reusable producer.
    @note Validation is fail-closed. Callers must never configure or compile a
      replacement producer when this function rejects an artifact.
    """

    cache = cmake_cache_values(build)

    def required_value(key: str) -> str:
        """@brief Return one required cache value for profile validation.

        @param key Exact CMake cache key.
        @return Serialized cache value.
        @throws RuntimeError If the key is absent.
        @note Empty values remain valid inputs for the caller to reject with a
          profile-specific diagnostic.
        """

        if key not in cache:
            raise RuntimeError(f"reusable producer cache is missing {key}")
        return cache[key]

    cached_source = Path(required_value("CMAKE_HOME_DIRECTORY")).resolve()
    if cached_source != repo:
        raise RuntimeError(
            "reusable producer source mismatch: "
            f"expected {repo}, got {cached_source}"
        )
    cached_build = Path(required_value("CMAKE_CACHEFILE_DIR")).resolve()
    if cached_build != build:
        raise RuntimeError(
            "reusable producer build mismatch: "
            f"expected {build}, got {cached_build}"
        )

    build_testing = required_value("BUILD_TESTING")
    if build_testing != "OFF":
        raise RuntimeError(
            f"reusable producer requires BUILD_TESTING=OFF, got {build_testing}"
        )
    build_ipc = required_value("PHOTOSPIDER_BUILD_IPC")
    if build_ipc != "OFF":
        raise RuntimeError(
            "reusable producer requires PHOTOSPIDER_BUILD_IPC=OFF, "
            f"got {build_ipc}"
        )

    configuration_types = cache.get("CMAKE_CONFIGURATION_TYPES", "")
    if configuration_types:
        available_configs = configuration_types.split(";")
        if config not in available_configs:
            raise RuntimeError(
                "reusable producer configuration mismatch: "
                f"requested {config}, available {available_configs}"
            )
    else:
        build_type = required_value("CMAKE_BUILD_TYPE")
        if build_type != config:
            raise RuntimeError(
                "reusable producer build type mismatch: "
                f"requested {config}, got {build_type}"
            )


def main() -> int:
    """@brief Build or reuse, install, and inspect an IPC-disabled package.

    @return Zero only when the embedded install remains usable; no IPC header,
      archive, executable, or exported target is advertised; optional disabled
      or unknown components remain absent without failing discovery; and a
      required ``ipc_client`` component fails discovery.
    @throws OSError For filesystem or process-start failures.
    @throws subprocess.CalledProcessError For configure/build/install failures.
    @throws ValueError If the requested work path could remove the repository.
    @throws RuntimeError If an artifact, export, component, or consumer
      condition contradicts the IPC-disabled package contract.
    @note Without ``--producer-build``, this durable product gate preserves the
      original fresh configure/build under ``work``. With that option, it
      strictly validates and reuses an external producer without configuring,
      compiling, or deleting it. Installation and all consumer checks remain
      transient under ``work``; no result/provenance report is written. On
      Darwin, every child configure inherits the selected producer's meaningful
      ``CMAKE_OSX_ARCHITECTURES`` value as one argv element; other platforms
      receive no macOS-specific option.
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
    consumer_source = work / "consumer"
    consumer_build = work / "consumer-build"
    missing_ipc_source = work / "missing-ipc-component"
    missing_ipc_build = work / "missing-ipc-component-build"
    optional_ipc_source = work / "optional-ipc-component"
    optional_ipc_build = work / "optional-ipc-component-build"
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
                    "photospider",
                    "--config",
                    args.config,
                    "--parallel",
                    "4",
                ],
                repo,
            )
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

        forwarder = (
            build / "generated" / "photospider_public_include" / "photospider" / "ipc"
        )
        forbidden_names = {
            "libphotospider_ipc_client.a",
            "libphotospider_ipc_client.lib",
            "photospider_ipc_client.lib",
            "photospiderd",
            "photospiderd.exe",
        }
        leaked = [
            path
            for path in prefix.rglob("*")
            if path.name in forbidden_names
            or "include/photospider/ipc" in path.as_posix()
        ]
        if leaked or forwarder.exists():
            raise RuntimeError(
                f"IPC-disabled build/install leaked artifacts: {leaked}, {forwarder}"
            )
        target_files = list(prefix.rglob("PhotospiderTargets*.cmake"))
        target_text = "\n".join(
            path.read_text(encoding="utf-8") for path in target_files
        )
        if "photospider_ipc" in target_text or "photospiderd" in target_text:
            raise RuntimeError("IPC-disabled export advertises IPC targets")

        optional_ipc_source.mkdir(parents=True)
        (optional_ipc_source / "CMakeLists.txt").write_text(
            "\n".join(
                [
                    "cmake_minimum_required(VERSION 3.16)",
                    "project(optional_ipc_component LANGUAGES CXX)",
                    "find_package(Photospider CONFIG",
                    "  OPTIONAL_COMPONENTS ipc_client unknown_optional)",
                    "if(NOT Photospider_FOUND)",
                    '  message(FATAL_ERROR "optional IPC lookup failed package")',
                    "endif()",
                    "if(Photospider_ipc_client_FOUND OR",
                    "   TARGET Photospider::photospider_ipc_client)",
                    '  message(FATAL_ERROR "disabled optional IPC was advertised")',
                    "endif()",
                    "if(Photospider_unknown_optional_FOUND)",
                    '  message(FATAL_ERROR "unknown optional component was found")',
                    "endif()",
                    "",
                ]
            ),
            encoding="utf-8",
        )
        run(
            [
                args.cmake_executable,
                "-S",
                str(optional_ipc_source),
                "-B",
                str(optional_ipc_build),
                f"-DCMAKE_PREFIX_PATH={prefix}",
                *child_architecture_arguments,
            ],
            repo,
        )

        missing_ipc_source.mkdir(parents=True)
        (missing_ipc_source / "CMakeLists.txt").write_text(
            "\n".join(
                [
                    "cmake_minimum_required(VERSION 3.16)",
                    "project(missing_ipc_component LANGUAGES CXX)",
                    "find_package(Photospider CONFIG REQUIRED",
                    "  COMPONENTS ipc_client)",
                    "",
                ]
            ),
            encoding="utf-8",
        )
        run_expect_failure(
            [
                args.cmake_executable,
                "-S",
                str(missing_ipc_source),
                "-B",
                str(missing_ipc_build),
                f"-DCMAKE_PREFIX_PATH={prefix}",
                *child_architecture_arguments,
            ],
            repo,
        )

        consumer_source.mkdir(parents=True)
        (consumer_source / "CMakeLists.txt").write_text(
            "\n".join(
                [
                    "cmake_minimum_required(VERSION 3.16)",
                    "project(ipc_disabled_consumer LANGUAGES CXX)",
                    "find_package(Photospider CONFIG REQUIRED)",
                    "add_executable(ipc_disabled_consumer main.cpp)",
                    "target_link_libraries(ipc_disabled_consumer",
                    "  PRIVATE Photospider::photospider)",
                    "",
                ]
            ),
            encoding="utf-8",
        )
        (consumer_source / "main.cpp").write_text(
            "\n".join(
                [
                    "#include <photospider/host/host.hpp>",
                    "int main() {",
                    "  auto host = ps::create_embedded_host();",
                    "  return host ? 0 : 1;",
                    "}",
                    "",
                ]
            ),
            encoding="utf-8",
        )
        run(
            [
                args.cmake_executable,
                "-S",
                str(consumer_source),
                "-B",
                str(consumer_build),
                f"-DCMAKE_PREFIX_PATH={prefix}",
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
            for name in ("ipc_disabled_consumer", "ipc_disabled_consumer.exe")
            for path in consumer_build.rglob(name)
            if "CMakeFiles" not in path.parts and path.is_file()
        ]
        if not executable_candidates:
            raise RuntimeError("IPC-disabled consumer executable was not found")
        executable = executable_candidates[0]
        run([str(executable)], repo)
        print("IPC-disabled install smoke: PASS", flush=True)
        return 0
    finally:
        remove_tree(work, repo)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:  # noqa: BLE001 - CTest needs one diagnostic.
        print(f"IPC-disabled install smoke: FAIL: {error}", file=sys.stderr)
        raise
