#!/usr/bin/env python3
"""Build and run the provider-disabled operation replacement profile."""

from __future__ import annotations

import argparse
import os
import pathlib
import shutil
import subprocess


def run(command: list[str], cwd: pathlib.Path) -> None:
    """@brief Run one required nested-build command with inherited output.

    @param command Executable and arguments passed directly without a shell.
    @param cwd Existing working directory for the child process.
    @return None after the command exits successfully.
    @throws OSError If the child process cannot be started.
    @throws subprocess.CalledProcessError If the command exits nonzero.
    @note The complete command is printed before execution, and inherited
      standard streams remain visible to CTest.
    """

    print("+", " ".join(command), flush=True)
    subprocess.run(command, cwd=cwd, check=True)


def resolve_work_directory(
    work: pathlib.Path, repo: pathlib.Path
) -> pathlib.Path:
    """@brief Resolve and validate one destructive nested-build directory.

    @param work Caller-selected directory whose prior tree may be removed.
    @param repo Photospider repository root that must remain untouched.
    @return Canonical work directory after every destructive-path guard passes.
    @throws OSError If either path cannot be resolved.
    @throws RuntimeError If symlink resolution cannot complete.
    @throws ValueError If work resolves to the repository, any repository
      ancestor, or any filesystem root.
    @note Resolution follows existing symlinks before comparison, so a link to
      the repository or one of its ancestors is rejected. The returned path is
      suitable only for caller-owned transient build content.
    """

    resolved_repo = repo.resolve()
    resolved_work = work.resolve()
    if resolved_work == resolved_repo or resolved_work in resolved_repo.parents:
        raise ValueError(
            "refusing to remove repository or ancestor as work path: "
            f"{resolved_work}"
        )
    if resolved_work.parent == resolved_work:
        raise ValueError(f"refusing to remove filesystem root: {resolved_work}")
    return resolved_work


def remove_work_tree(work: pathlib.Path, repo: pathlib.Path) -> pathlib.Path:
    """@brief Remove one validated nested-build tree without hiding failure.

    @param work Caller-selected directory whose previous contents must vanish.
    @param repo Photospider repository root protected from recursive removal.
    @return Canonical work directory, absent when this function returns.
    @throws OSError If path inspection, resolution, or recursive removal fails.
    @throws ValueError If work resolves to a protected destructive path.
    @throws RuntimeError If symlink resolution cannot complete or recursive
      removal returns while the tree remains.
    @note Validation is repeated in this destructive helper so callers cannot
      accidentally separate safety checks from deletion. The helper never
      creates the returned directory.
    """

    resolved_work = resolve_work_directory(work, repo)
    if resolved_work.exists() or resolved_work.is_symlink():
        shutil.rmtree(resolved_work)
    if resolved_work.exists() or resolved_work.is_symlink():
        raise RuntimeError(
            f"nested provider build directory still exists: {resolved_work}"
        )
    return resolved_work


def cmake_cache_values(build: pathlib.Path) -> dict[str, str]:
    """@brief Read exact assignments from one nested CMake cache.

    @param build Configured nested build directory containing CMakeCache.txt.
    @return Mapping from cache keys to their final serialized values.
    @throws OSError If the cache cannot be read.
    @throws UnicodeError If the cache is not valid UTF-8 text.
    @throws RuntimeError If the nested build has no regular CMake cache file.
    @note Comment and malformed lines are ignored; later duplicate keys win,
      matching CMake's effective final assignment.
    """

    cache_path = build / "CMakeCache.txt"
    if not cache_path.is_file():
        raise RuntimeError(f"nested provider build has no cache: {cache_path}")
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


def configured_test_executable(
    build: pathlib.Path, configuration: str
) -> pathlib.Path:
    """@brief Resolve the focused test path from nested generator state.

    @param build Configured nested build directory.
    @param configuration Exact configuration requested from the build tool.
    @return Expected focused test executable for the cached generator mode.
    @throws OSError If the nested CMake cache cannot be read.
    @throws UnicodeError If the nested CMake cache is not valid UTF-8 text.
    @throws RuntimeError If configuration metadata is missing or contradicts
      the requested configuration.
    @note A nonempty CMAKE_CONFIGURATION_TYPES value is the sole authority for
      multi-config layout. Platform identity affects only the executable
      suffix and never the presence of a configuration directory.
    """

    cache = cmake_cache_values(build)
    configuration_types = cache.get("CMAKE_CONFIGURATION_TYPES", "")
    if configuration_types:
        available = [
            candidate.strip()
            for candidate in configuration_types.split(";")
            if candidate.strip()
        ]
        if not configuration:
            raise RuntimeError(
                "multi-config nested provider build requires --config"
            )
        if configuration not in available:
            raise RuntimeError(
                "nested provider build configuration mismatch: "
                f"requested {configuration}, available {available}"
            )
        output_directory = build / "tests" / configuration
    else:
        if "CMAKE_BUILD_TYPE" not in cache:
            raise RuntimeError(
                "single-config nested provider build cache is missing "
                "CMAKE_BUILD_TYPE"
            )
        build_type = cache["CMAKE_BUILD_TYPE"]
        if not build_type:
            raise RuntimeError(
                "single-config nested provider build has an empty "
                "CMAKE_BUILD_TYPE"
            )
        if build_type != configuration:
            raise RuntimeError(
                "nested provider build type mismatch: "
                f"requested {configuration}, got {build_type}"
            )
        output_directory = build / "tests"

    executable_suffix = ".exe" if os.name == "nt" else ""
    return output_directory / (
        "test_optional_opencv_operation_provider" + executable_suffix
    )


def main() -> int:
    """@brief Configure, build, and run the provider-disabled regression.

    @return Zero after the focused test executable succeeds.
    @throws OSError If path handling or command startup fails.
    @throws SystemExit If command-line parsing rejects the invocation.
    @throws UnicodeError If the nested CMake cache is not valid UTF-8 text.
    @throws ValueError If the destructive work path resolves to a protected
      repository or filesystem location.
    @throws RuntimeError If cleanup, cache metadata, configuration selection,
      or executable discovery violates the nested-build contract.
    @throws subprocess.CalledProcessError If configure, build, or test
      execution exits nonzero.
    @note The function removes only the validated caller-owned work tree before
      configuration. It leaves the successful nested build available to CTest
      cleanup and writes no separate report or provenance artifact.
    """

    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", required=True, type=pathlib.Path)
    parser.add_argument("--work", required=True, type=pathlib.Path)
    parser.add_argument("--cmake-executable", required=True)
    parser.add_argument("--config", default="RelWithDebInfo")
    args = parser.parse_args()

    repo = args.repo.resolve()
    work = remove_work_tree(args.work, repo)
    configuration = args.config or "RelWithDebInfo"

    run(
        [
            args.cmake_executable,
            "-S",
            str(repo),
            "-B",
            str(work),
            f"-DCMAKE_BUILD_TYPE={configuration}",
            "-DBUILD_TESTING=ON",
            "-DPHOTOSPIDER_BUILD_IPC=OFF",
            "-DPHOTOSPIDER_BUILD_OPENCV_OPERATION_PROVIDER=OFF",
        ],
        repo,
    )

    build_command = [
        args.cmake_executable,
        "--build",
        str(work),
        "--target",
        "test_optional_opencv_operation_provider",
        "-j",
        "4",
    ]
    build_command.extend(["--config", configuration])
    run(build_command, repo)

    executable = configured_test_executable(work, configuration)
    if not executable.is_file():
        raise RuntimeError(
            "nested provider test executable is missing for cached "
            f"configuration: {executable}"
        )
    run([str(executable)], repo)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
