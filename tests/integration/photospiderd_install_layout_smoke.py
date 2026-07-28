#!/usr/bin/env python3
"""Execute installed photospiderd across supported GNUInstallDirs layouts."""

from __future__ import annotations

import argparse
import platform
import shlex
import shutil
import subprocess
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class InstallLayout:
    """@brief Describe one isolated daemon/runtime install layout.

    @param name Stable case label used only for diagnostics and directory names.
    @param bindir Configured ``CMAKE_INSTALL_BINDIR`` value.
    @param libdir Configured ``CMAKE_INSTALL_LIBDIR`` value.
    @throws None The record retains immutable strings only.
    @note Absolute values are always generated below the validated work root.
    """

    name: str
    bindir: str
    libdir: str


def strict_remove_tree(path: Path) -> None:
    """@brief Remove one validated transient directory without following it.

    @param path Exact work directory previously proven to be below build-root.
    @return None after the directory is absent.
    @throws RuntimeError If ``path`` is a symlink or remains after removal.
    @throws OSError If filesystem inspection or recursive removal fails.
    @note ``shutil.rmtree`` removes symlinks contained by a real directory
      without following their targets. The root itself must never be a symlink.
    """

    if path.is_symlink():
        raise RuntimeError(f"refusing to remove symlink work root: {path}")
    if path.exists():
        shutil.rmtree(path)
    if path.exists() or path.is_symlink():
        raise RuntimeError(f"transient work root still exists: {path}")


def validate_work_root(repo: Path, build_root: Path, work: Path) -> None:
    """@brief Prove that destructive cleanup is confined to one build tree.

    @param repo Canonical source repository that must never be removed.
    @param build_root Canonical outer producer build directory.
    @param work Canonical transient directory selected for this smoke.
    @return None when ``work`` is a strict descendant of ``build_root``.
    @throws ValueError If a source/ancestor/build-root boundary is unsafe.
    @note The caller separately rejects a symlink at the original work path
      before canonicalization.
    """

    if build_root == repo or build_root in repo.parents:
        raise ValueError(
            f"refusing source tree or ancestor as build root: {build_root}"
        )
    if work == repo or work in repo.parents:
        raise ValueError(f"refusing source tree or ancestor as work root: {work}")
    if work == build_root or build_root not in work.parents:
        raise ValueError(
            "work root must be a strict build-root descendant: "
            f"build_root={build_root}, work={work}"
        )


def run_checked(command: list[str], cwd: Path) -> None:
    """@brief Run one visible child command and require success.

    @param command Executable and arguments passed directly without a shell.
    @param cwd Existing child working directory.
    @return None after a zero child exit status.
    @throws OSError If the process cannot start.
    @throws RuntimeError If the child exits with a nonzero status.
    @note Standard output and error are inherited for direct CTest diagnostics.
    """

    print("$ " + shlex.join(command), flush=True)
    completed = subprocess.run(command, cwd=cwd, check=False)
    if completed.returncode != 0:
        raise RuntimeError(
            f"command exited with {completed.returncode}: {shlex.join(command)}"
        )


def generator_is_multi_config(generator: str) -> bool:
    """@brief Classify supported CMake multi-configuration generators.

    @param generator Exact generator name supplied by the outer CMake project.
    @return True for Xcode, Visual Studio, or explicit Multi-Config generators.
    @throws None This function performs only string comparisons.
    @note The smoke is registered only for the Darwin/Linux IPC product, but
      Visual Studio remains classified so direct helper use is unsurprising.
    """

    return (
        generator == "Xcode"
        or generator.startswith("Visual Studio ")
        or "Multi-Config" in generator
    )


def configured_layouts(work: Path) -> tuple[InstallLayout, ...]:
    """@brief Construct the maintained GNUInstallDirs execution matrix.

    @param work Validated transient root owning every absolute destination.
    @return Nested-relative, absolute-LIBDIR, and absolute-BINDIR cases.
    @throws None Path composition has no filesystem side effect.
    @note Default relative ``bin``/``lib`` remains covered by
      ``StaticProductConsumerSmoke``. Mixed absolute-BINDIR/relative-LIBDIR is
      installed with its configured prefix because an install-time ``--prefix``
      override cannot relocate an absolute destination.
    """

    return (
        InstallLayout(
            name="nested-relative",
            bindir="libexec/photospider",
            libdir="lib64",
        ),
        InstallLayout(
            name="absolute-libdir",
            bindir="bin",
            libdir=str(work / "absolute-libdir" / "runtime"),
        ),
        InstallLayout(
            name="absolute-bindir",
            bindir=str(work / "absolute-bindir" / "daemon"),
            libdir="lib64",
        ),
    )


def installed_daemon_path(
    prefix: Path, bindir: str, platform_system: str
) -> Path:
    """@brief Resolve the installed daemon from GNUInstallDirs semantics.

    @param prefix Configured ``CMAKE_INSTALL_PREFIX`` for the case.
    @param bindir Configured relative or absolute ``CMAKE_INSTALL_BINDIR``.
    @param platform_system Value returned by ``platform.system()``.
    @return Exact installed ``photospiderd`` executable path.
    @throws None Path composition has no filesystem side effect.
    @note Absolute destinations ignore the install prefix exactly as CMake
      install rules do.
    """

    directory = Path(bindir)
    if not directory.is_absolute():
        directory = prefix / directory
    executable = (
        "photospiderd.exe" if platform_system == "Windows" else "photospiderd"
    )
    return directory / executable


def run_layout(
    layout: InstallLayout,
    *,
    repo: Path,
    work: Path,
    cmake_executable: str,
    generator: str,
    config: str,
    osx_architectures: str,
    platform_system: str,
) -> None:
    """@brief Configure, build, install, and execute one isolated layout.

    @param layout GNUInstallDirs values owned by this case.
    @param repo Canonical Photospider source repository.
    @param work Validated matrix work root.
    @param cmake_executable Exact CMake executable selected by the outer build.
    @param generator Outer generator reused by the child producer.
    @param config Requested build configuration, possibly empty.
    @param osx_architectures Darwin architecture list, possibly empty.
    @param platform_system Host platform name.
    @return None after installed ``photospiderd --help`` succeeds.
    @throws OSError If filesystem or process startup fails.
    @throws RuntimeError If configure, build, install, or daemon help fails.
    @note Every child directory is case-local. The shared help driver removes
      LD/DYLD and related loader overrides immediately before real execution.
    """

    case_root = work / layout.name
    build = case_root / "build"
    prefix = case_root / "prefix"
    case_root.mkdir(parents=True)

    configure_command = [
        cmake_executable,
        "-S",
        str(repo),
        "-B",
        str(build),
        "-DBUILD_TESTING=OFF",
        "-DPHOTOSPIDER_ENABLE_OPENCV=OFF",
        "-DPHOTOSPIDER_ENABLE_YAML=OFF",
        "-DPHOTOSPIDER_BUILD_OPENCV_OPERATION_PROVIDER=OFF",
        "-DPHOTOSPIDER_BUILD_OPENCV_OPERATION_PLUGINS=OFF",
        "-DPHOTOSPIDER_BUILD_GRAPH_CLI=OFF",
        "-DPHOTOSPIDER_BUILD_IPC=ON",
        f"-DCMAKE_INSTALL_PREFIX={prefix}",
        f"-DCMAKE_INSTALL_BINDIR={layout.bindir}",
        f"-DCMAKE_INSTALL_LIBDIR={layout.libdir}",
    ]
    if generator:
        configure_command.extend(["-G", generator])
    if config and not generator_is_multi_config(generator):
        configure_command.append(f"-DCMAKE_BUILD_TYPE={config}")
    if platform_system == "Darwin" and osx_architectures:
        configure_command.append(
            f"-DCMAKE_OSX_ARCHITECTURES={osx_architectures}"
        )
    run_checked(configure_command, case_root)

    build_command = [
        cmake_executable,
        "--build",
        str(build),
        "--target",
        "photospiderd",
    ]
    if config:
        build_command.extend(["--config", config])
    run_checked(build_command, case_root)

    install_command = [cmake_executable, "--install", str(build)]
    if config:
        install_command.extend(["--config", config])
    run_checked(install_command, case_root)

    daemon = installed_daemon_path(prefix, layout.bindir, platform_system)
    if not daemon.is_file():
        raise RuntimeError(f"installed daemon is absent: {daemon}")
    run_checked(
        [
            cmake_executable,
            f"-DPHOTOSPIDERD={daemon}",
            "-P",
            str(
                repo
                / "tests"
                / "integration"
                / "photospiderd_capability_help.cmake"
            ),
        ],
        case_root,
    )
    print(f"layout {layout.name} installed daemon help passed", flush=True)


def main() -> int:
    """@brief Execute and clean the complete installed-layout matrix.

    @return Zero after all maintained layouts execute successfully.
    @throws ValueError If caller paths could escape the outer build tree.
    @throws OSError If filesystem or process startup fails.
    @throws RuntimeError If cleanup or one required behavior fails.
    @note Cleanup runs in ``finally`` after the work root is created, so both
      success and failure discard every child build/install directory.
    """

    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", required=True)
    parser.add_argument("--build-root", required=True)
    parser.add_argument("--work", required=True)
    parser.add_argument("--cmake-executable", default="cmake")
    parser.add_argument("--generator", default="")
    parser.add_argument("--config", default="")
    parser.add_argument("--osx-architectures", default="")
    args = parser.parse_args()

    platform_system = platform.system()
    if platform_system not in {"Darwin", "Linux"}:
        raise RuntimeError(
            f"photospiderd install layout smoke is unsupported on {platform_system}"
        )

    repo = Path(args.repo).resolve(strict=True)
    build_root = Path(args.build_root).resolve(strict=True)
    raw_work = Path(args.work).absolute()
    if raw_work.is_symlink():
        raise ValueError(f"work root must not be a symlink: {raw_work}")
    work = raw_work.resolve(strict=False)
    validate_work_root(repo, build_root, work)

    strict_remove_tree(work)
    work.mkdir(parents=True)
    try:
        layouts = configured_layouts(work)
        for layout in layouts:
            run_layout(
                layout,
                repo=repo,
                work=work,
                cmake_executable=args.cmake_executable,
                generator=args.generator,
                config=args.config,
                osx_architectures=args.osx_architectures,
                platform_system=platform_system,
            )
        print(f"all {len(layouts)} install layouts passed", flush=True)
    finally:
        strict_remove_tree(work)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
