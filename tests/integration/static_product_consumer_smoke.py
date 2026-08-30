#!/usr/bin/env python3
"""Install Photospider and validate its kernel-only package boundary."""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
from pathlib import Path, PurePosixPath


def run_checked(command: list[str], cwd: Path) -> subprocess.CompletedProcess[str]:
    """@brief Run one visible child command and require success.

    @param command Executable and arguments passed directly without a shell.
    @param cwd Existing child working directory.
    @return Completed process with combined output after a zero exit status.
    @throws OSError If the process cannot start.
    @throws RuntimeError If the child exits with a nonzero status.
    @note Output is captured and replayed so CTest retains exact diagnostics.
    """

    print("$ " + " ".join(command), flush=True)
    completed = subprocess.run(
        command,
        cwd=cwd,
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    print(completed.stdout, end="", flush=True)
    if completed.returncode != 0:
        raise RuntimeError(
            f"command exited with {completed.returncode}: {' '.join(command)}"
        )
    return completed


def remove_transient_tree(path: Path, build_root: Path) -> None:
    """@brief Remove one validated transient package-smoke directory.

    @param path Exact work directory selected by the caller.
    @param build_root Configured producer build that must contain ``path``.
    @return None after the directory is absent.
    @throws RuntimeError If the target is a symlink or escapes the build tree.
    @throws OSError If filesystem removal fails.
    @note The producer build itself and source tree are never removed.
    """

    unresolved = path.absolute()
    if unresolved.is_symlink():
        raise RuntimeError(f"refusing symlink smoke root: {unresolved}")
    resolved_root = build_root.resolve(strict=True)
    resolved = unresolved.resolve(strict=False)
    if resolved == resolved_root or resolved_root not in resolved.parents:
        raise RuntimeError(f"unsafe smoke root: {resolved}")
    if resolved.exists():
        shutil.rmtree(resolved)


def configured_public_headers(build: Path) -> list[str]:
    """@brief Read the producer's exact installable public-header inventory.

    @param build Configured producer build tree.
    @return Sorted canonical paths relative to the future install prefix.
    @throws OSError If the generated inventory cannot be read.
    @throws RuntimeError If an entry is blank, duplicated, or noncanonical.
    @note This is the same allowlist consumed by CMake install rules.
    """

    inventory = (
        build
        / "generated"
        / "ci_inventory"
        / "installable_public_headers.txt"
    )
    rows = inventory.read_text(encoding="utf-8").splitlines()
    if not rows or len(rows) != len(set(rows)) or any(not row for row in rows):
        raise RuntimeError("configured public-header inventory is invalid")
    expected: list[str] = []
    for row in rows:
        path = PurePosixPath(row)
        if (
            path.as_posix() != row
            or path.is_absolute()
            or path.parts[:2] != ("include", "photospider")
            or any(part in {"", ".", ".."} for part in path.parts)
        ):
            raise RuntimeError(f"noncanonical public-header entry: {row}")
        expected.append(row)
    return sorted(expected)


def installed_files(prefix: Path) -> list[str]:
    """@brief Inventory every regular file below one isolated installation.

    @param prefix Temporary CMake install prefix.
    @return Sorted POSIX paths relative to ``prefix``.
    @throws OSError If traversal or metadata access fails.
    @note Symlinks are not treated as independent regular package artifacts.
    """

    return sorted(
        path.relative_to(prefix).as_posix()
        for path in prefix.rglob("*")
        if path.is_file()
    )


def write_consumer(source: Path, include_lines: list[str]) -> None:
    """@brief Write one external embedded-Host package consumer.

    @param source New transient consumer source directory.
    @param include_lines Exact installed public headers to compile together.
    @return None after CMake and C++ sources are complete.
    @throws OSError If directory or file creation fails.
    @note The project compiles every installed header and exercises copied Host
      service values at runtime.
    """

    source.mkdir(parents=True)
    (source / "CMakeLists.txt").write_text(
        """cmake_minimum_required(VERSION 3.16)
project(PhotospiderInstalledKernelConsumer LANGUAGES CXX)
find_package(Photospider CONFIG REQUIRED COMPONENTS embedded operation_opencv)
if(NOT Photospider_embedded_FOUND OR NOT TARGET Photospider::photospider)
  message(FATAL_ERROR "installed embedded kernel target is absent")
endif()
add_executable(photospider_consumer main.cpp)
target_compile_features(photospider_consumer PRIVATE cxx_std_17)
target_link_libraries(photospider_consumer PRIVATE
  Photospider::photospider
  Photospider::operation_opencv)
""",
        encoding="utf-8",
    )
    (source / "main.cpp").write_text(
        "\n".join(
            [
                "#include <memory>",
                "#include <string>",
                "#include <vector>",
                "",
                *include_lines,
                "",
                "/**",
                " * @brief Exercises the installed kernel-only Host package.",
                " * @return Zero when embedded construction and copied service",
                " *         inventories retain their public contracts.",
                " * @throws std::bad_alloc on unrecoverable allocation failure.",
                " */",
                "int main() {",
                "  std::unique_ptr<ps::Host> host = ps::create_embedded_host();",
                "  if (!host) {",
                "    return 1;",
                "  }",
                "  const auto policies = host->policy_available_types();",
                "  const auto executions = host->execution_available_types();",
                "  if (!policies.status.ok || !executions.status.ok) {",
                "    return 2;",
                "  }",
                "  const std::vector<std::string> expected_policies =",
                '      {"interactive", "throughput"};',
                "  const std::vector<std::string> expected_executions =",
                '      {"cpu", "gpu_pipeline", "serial_debug"};',
                "  return policies.value == expected_policies &&",
                "                 executions.value == expected_executions",
                "             ? 0",
                "             : 3;",
                "}",
                "",
            ]
        ),
        encoding="utf-8",
    )


def main() -> int:
    """@brief Build, install, and consume the kernel package in isolation.

    @return Zero only when positive package and negative ownership checks pass.
    @throws OSError or RuntimeError for filesystem, command, or contract failure.
    @note Cleanup runs in ``finally`` and remains confined below producer build.
    """

    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", required=True)
    parser.add_argument("--build", required=True)
    parser.add_argument("--work", required=True)
    parser.add_argument("--cmake-executable", default="cmake")
    parser.add_argument("--config", default="")
    args = parser.parse_args()

    repo = Path(args.repo).resolve(strict=True)
    build = Path(args.build).resolve(strict=True)
    work = Path(args.work).absolute()
    remove_transient_tree(work, build)
    work.mkdir(parents=True)
    prefix = work / "prefix"
    consumer_source = work / "consumer-source"
    consumer_build = work / "consumer-build"
    try:
        build_product = [
            args.cmake_executable,
            "--build",
            str(build),
            "--target",
            "photospider",
            "photospider_operation_runtime",
            "photospider_operation_opencv",
        ]
        if args.config:
            build_product.extend(["--config", args.config])
        run_checked(build_product, repo)

        install = [
            args.cmake_executable,
            "--install",
            str(build),
            "--prefix",
            str(prefix),
        ]
        if args.config:
            install.extend(["--config", args.config])
        run_checked(install, repo)

        files = installed_files(prefix)
        actual_headers = sorted(
            path for path in files if path.startswith("include/")
        )
        expected_headers = configured_public_headers(build)
        if actual_headers != expected_headers:
            raise RuntimeError("installed public headers differ from allowlist")
        package_files = list(prefix.glob("**/cmake/Photospider/*.cmake"))
        if not package_files:
            raise RuntimeError("installed Photospider package files are absent")
        package_text = "\n".join(
            path.read_text(encoding="utf-8") for path in package_files
        )
        if (
            str(repo) in package_text
            or "/src/lib" in package_text
        ):
            raise RuntimeError("installed package export retained private ownership")

        include_lines = [
            f"#include <{path.removeprefix('include/')}>"
            for path in expected_headers
        ]
        write_consumer(consumer_source, include_lines)
        configure = [
            args.cmake_executable,
            "-S",
            str(consumer_source),
            "-B",
            str(consumer_build),
            f"-DCMAKE_PREFIX_PATH={prefix}",
        ]
        if args.config and not os.environ.get("CMAKE_CONFIGURATION_TYPES"):
            configure.append(f"-DCMAKE_BUILD_TYPE={args.config}")
        run_checked(configure, work)
        build_consumer = [args.cmake_executable, "--build", str(consumer_build)]
        if args.config:
            build_consumer.extend(["--config", args.config])
        run_checked(build_consumer, work)
        candidates = [consumer_build / "photospider_consumer"]
        if args.config:
            candidates.insert(
                0, consumer_build / args.config / "photospider_consumer"
            )
        executable = next((path for path in candidates if path.is_file()), None)
        if executable is None:
            raise RuntimeError("installed kernel consumer executable is absent")
        run_checked([str(executable)], work)

    finally:
        remove_transient_tree(work, build)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
