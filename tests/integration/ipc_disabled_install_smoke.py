#!/usr/bin/env python3
"""Verify an IPC-disabled install advertises only the embedded product."""

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


def main() -> int:
    """@brief Configure, build, install, and inspect an IPC-disabled package.

    @return Zero only when embedded install remains usable and no IPC header,
      archive, executable, or exported target is advertised.
    @throws OSError For filesystem or process-start failures.
    @throws subprocess.CalledProcessError For configure/build/install failures.
    @note This durable product gate creates only a transient normal CMake tree;
      it writes no result/provenance report.
    """

    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", required=True)
    parser.add_argument("--work", required=True)
    parser.add_argument("--cmake-executable", default="cmake")
    parser.add_argument("--config", default="RelWithDebInfo")
    args = parser.parse_args()

    repo = Path(args.repo).resolve()
    work = Path(args.work).resolve()
    remove_tree(work, repo)
    build = work / "build"
    prefix = work / "install"
    consumer_source = work / "consumer"
    consumer_build = work / "consumer-build"
    missing_ipc_source = work / "missing-ipc-component"
    missing_ipc_build = work / "missing-ipc-component-build"
    optional_ipc_source = work / "optional-ipc-component"
    optional_ipc_build = work / "optional-ipc-component-build"
    try:
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
