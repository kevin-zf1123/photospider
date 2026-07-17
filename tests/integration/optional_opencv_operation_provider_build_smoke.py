#!/usr/bin/env python3
"""Build and run the provider-disabled operation replacement profile."""

from __future__ import annotations

import argparse
import pathlib
import shutil
import subprocess
import sys


def run(command: list[str], cwd: pathlib.Path) -> None:
    """Run one checked subprocess with its complete command visible."""

    print("+", " ".join(command), flush=True)
    subprocess.run(command, cwd=cwd, check=True)


def main() -> int:
    """Configure, build, and execute the focused provider-disabled test."""

    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", required=True, type=pathlib.Path)
    parser.add_argument("--work", required=True, type=pathlib.Path)
    parser.add_argument("--cmake-executable", required=True)
    parser.add_argument("--config", default="")
    args = parser.parse_args()

    repo = args.repo.resolve()
    work = args.work.resolve()
    shutil.rmtree(work, ignore_errors=True)

    run(
        [
            args.cmake_executable,
            "-S",
            str(repo),
            "-B",
            str(work),
            "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
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
    if args.config:
        build_command.extend(["--config", args.config])
    run(build_command, repo)

    executable = work / "tests" / "test_optional_opencv_operation_provider"
    if sys.platform == "win32":
        configuration = args.config or "RelWithDebInfo"
        executable = work / "tests" / configuration / (
            "test_optional_opencv_operation_provider.exe"
        )
    run([str(executable)], repo)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
