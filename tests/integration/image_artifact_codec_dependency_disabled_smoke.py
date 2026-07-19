#!/usr/bin/env python3
"""Configure and build the dependency-neutral image codec contract profile."""

from __future__ import annotations

import argparse
import pathlib
import subprocess

from optional_opencv_operation_provider_build_smoke import remove_work_tree


def run(command: list[str], cwd: pathlib.Path) -> None:
    """@brief Run one required nested-build command with inherited output.

    @param command Executable and arguments passed directly without a shell.
    @param cwd Existing working directory for the child process.
    @return None after the command exits successfully.
    @throws OSError If the child process cannot be started.
    @throws subprocess.CalledProcessError If the command exits nonzero.
    @note The command is printed before execution and no result artifact is
      retained beyond CTest's captured stream.
    """

    print("+", " ".join(command), flush=True)
    subprocess.run(command, cwd=cwd, check=True)


def main() -> int:
    """@brief Build the fake-codec cache contract with the operation provider off.

    @return Zero after configure, focused build, and fake-codec tests succeed.
    @throws OSError If path resolution or command startup fails.
    @throws SystemExit If argument parsing rejects the invocation.
    @throws ValueError If the destructive work path is unsafe.
    @throws RuntimeError If safe cleanup fails or the focused executable is
      missing after build.
    @throws subprocess.CalledProcessError If configure, build, or test execution
      exits nonzero.
    @note This profile intentionally disables the repository OpenCV operation
      provider while retaining the separately configured OpenCV artifact adapter.
      `DependencyDisabledInstallSmoke` separately validates complete
      OpenCV-free product composition.
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
    run(
        [
            args.cmake_executable,
            "--build",
            str(work),
            "--target",
            "test_kernel_contracts",
            "-j",
            "2",
            "--config",
            configuration,
        ],
        repo,
    )

    executable = work / "tests" / "test_kernel_contracts"
    if not executable.is_file():
        executable = work / "tests" / configuration / "test_kernel_contracts"
    run(
        [
            str(executable),
            "--gtest_filter=CacheSemantics.InjectedCodec*",
        ],
        repo,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
