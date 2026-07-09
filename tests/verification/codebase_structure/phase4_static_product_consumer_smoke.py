#!/usr/bin/env python3
"""Install Photospider and verify an external CMake consumer can link it."""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
from pathlib import Path
from typing import Any


def run_command(
    command: list[str], cwd: Path, log_path: Path, env: dict[str, str] | None = None
) -> int:
    """Run a command, write combined output, and return its exit code."""

    proc = subprocess.run(
        command,
        cwd=cwd,
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        env=env,
    )
    log_path.write_text(
        "$ " + " ".join(command) + "\n" + proc.stdout,
        encoding="utf-8",
    )
    return proc.returncode


def public_header_includes(repo: Path) -> list[str]:
    """Return include directives for every installable Photospider header."""

    public_root = repo / "include" / "photospider"
    headers = sorted(
        path.relative_to(repo / "include").as_posix()
        for path in public_root.rglob("*")
        if path.is_file() and path.suffix in {".h", ".hpp", ".hh", ".hxx"}
    )
    return [f"#include <{header}>" for header in headers]


def write_consumer_project(repo: Path, source_dir: Path) -> list[str]:
    """Create a minimal embedded-frontend consumer project."""

    source_dir.mkdir(parents=True, exist_ok=True)
    include_lines = public_header_includes(repo)
    (source_dir / "CMakeLists.txt").write_text(
        "\n".join(
            [
                "cmake_minimum_required(VERSION 3.16)",
                "project(photospider_consumer_smoke LANGUAGES CXX)",
                "find_package(Photospider CONFIG REQUIRED)",
                "add_executable(photospider_consumer main.cpp)",
                "target_link_libraries(photospider_consumer",
                "    PRIVATE Photospider::photospider)",
                "",
            ]
        ),
        encoding="utf-8",
    )
    (source_dir / "main.cpp").write_text(
        "\n".join(
            [
                "#include <cstddef>",
                "#include <memory>",
                "",
                *include_lines,
                "",
                "int main() {",
                "  auto host = ps::create_embedded_host();",
                "  if (!host) {",
                "    return 1;",
                "  }",
                "  const std::size_t step = ps::aligned_image_buffer_step(",
                "      8, 4, ps::DataType::FLOAT32);",
                "  auto image = ps::make_aligned_cpu_image_buffer(",
                "      2, 2, 4, ps::DataType::FLOAT32);",
                "  return step == 128 && image.data ? 0 : 2;",
                "}",
                "",
            ]
        ),
        encoding="utf-8",
    )
    return include_lines


def cmake_cache_value(build: Path, key: str) -> str:
    """Read one value from CMakeCache.txt when it is present."""

    cache = build / "CMakeCache.txt"
    if not cache.is_file():
        return ""
    prefix = f"{key}:"
    for line in cache.read_text(encoding="utf-8").splitlines():
        if not line.startswith(prefix):
            continue
        return line.split("=", 1)[1] if "=" in line else ""
    return ""


def installed_headers(prefix: Path) -> list[str]:
    """Return installed header files relative to the prefix."""

    include_dir = prefix / "include"
    if not include_dir.exists():
        return []
    return sorted(
        path.relative_to(prefix).as_posix()
        for path in include_dir.rglob("*")
        if path.is_file()
    )


def inspect_install_tree(repo: Path, prefix: Path) -> dict[str, Any]:
    """Inspect installed files and exported target metadata."""

    headers = installed_headers(prefix)
    targets_path = (
        prefix / "lib" / "cmake" / "Photospider" / "PhotospiderTargets.cmake"
    )
    config_path = (
        prefix / "lib" / "cmake" / "Photospider" / "PhotospiderConfig.cmake"
    )
    target_text = targets_path.read_text(encoding="utf-8") if targets_path.exists() else ""
    config_text = config_path.read_text(encoding="utf-8") if config_path.exists() else ""
    unexpected_headers = [
        header for header in headers if not header.startswith("include/photospider/")
    ]
    source_root = repo.as_posix()
    return {
        "headers": headers,
        "unexpected_headers": unexpected_headers,
        "archive_exists": (prefix / "lib" / "libphotospider.a").is_file(),
        "config_exists": config_path.is_file(),
        "targets_exists": targets_path.is_file(),
        "export_mentions_namespace_target": "Photospider::photospider" in target_text,
        "export_has_static_compile_definition": "PHOTOSPIDER_STATIC" in target_text,
        "export_has_install_include_dir": "${_IMPORT_PREFIX}/include" in target_text,
        "export_omits_source_root": source_root not in target_text
        and source_root not in config_text,
        "export_omits_src_include_root": "/src" not in target_text,
        "config_finds_opencv": "find_dependency(OpenCV" in config_text,
        "config_finds_yaml_cpp": "find_dependency(yaml-cpp)" in config_text,
        "config_finds_threads": "find_dependency(Threads)" in config_text,
    }


def make_compare(actual: dict[str, Any]) -> tuple[bool, str]:
    """Create PASS/FAIL comparison text from real smoke-test outputs."""

    install = actual["install_tree"]
    commands = actual["commands"]
    compiled_headers = sorted(
        "include/" + line.removeprefix("#include <").removesuffix(">")
        for line in actual["consumer"]["compiled_public_headers"]
    )
    checks = {
        "photospider target build succeeded": commands["build_photospider"] == 0,
        "install command succeeded": commands["install"] == 0,
        "installed static archive exists": install["archive_exists"],
        "package config and targets exist": install["config_exists"]
        and install["targets_exists"],
        "only include/photospider headers are installed": install[
            "unexpected_headers"
        ]
        == [],
        "consumer compiles every installed public header": compiled_headers
        == install["headers"],
        "exported namespace target exists": install[
            "export_mentions_namespace_target"
        ],
        "exported target carries PHOTOSPIDER_STATIC": install[
            "export_has_static_compile_definition"
        ],
        "exported target uses install include root": install[
            "export_has_install_include_dir"
        ],
        "exported package is source-tree clean": install["export_omits_source_root"]
        and install["export_omits_src_include_root"],
        "package config finds dependencies": install["config_finds_opencv"]
        and install["config_finds_yaml_cpp"]
        and install["config_finds_threads"],
        "consumer configure succeeded": commands["consumer_configure"] == 0,
        "consumer build succeeded": commands["consumer_build"] == 0,
        "consumer executable ran successfully": commands["consumer_run"] == 0,
    }
    passed = all(checks.values())
    lines = ["phase4_static_product_consumer_smoke"]
    lines.extend(
        f"{'PASS' if ok else 'FAIL'} {name}" for name, ok in checks.items()
    )
    if install["unexpected_headers"]:
        lines.append("unexpected installed headers:")
        lines.extend(f"- {header}" for header in install["unexpected_headers"])
    lines.append(f"overall={'PASS' if passed else 'FAIL'}")
    return passed, "\n".join(lines) + "\n"


def make_summary(out: Path, actual: dict[str, Any], passed: bool) -> str:
    """Create a reader-facing smoke-test summary."""

    return "\n".join(
        [
            "# codebase-refactor phase-4 static product consumer smoke",
            "",
            "## Test objective",
            "",
            "Install the static Photospider package, consume it from a separate",
            "CMake project with `find_package(Photospider CONFIG REQUIRED)`,",
            "link `Photospider::photospider`, compile every installed public",
            "Photospider header from the package include root, run a small",
            "embedded frontend program, and verify the install tree exposes",
            "only `include/photospider` headers.",
            "",
            "## Evidence files",
            "",
            f"- `expected.json`: `{out / 'expected.json'}`",
            f"- `actual.json`: `{out / 'actual.json'}`",
            f"- `compare.log`: `{out / 'compare.log'}`",
            f"- `build_photospider.log`: `{out / 'build_photospider.log'}`",
            f"- `install.log`: `{out / 'install.log'}`",
            f"- `consumer_configure.log`: `{out / 'consumer_configure.log'}`",
            f"- `consumer_build.log`: `{out / 'consumer_build.log'}`",
            f"- `consumer_run.log`: `{out / 'consumer_run.log'}`",
            "",
            "## Result",
            "",
            "- Public headers compiled by consumer: "
            f"{len(actual['consumer']['compiled_public_headers'])}",
            f"- Installed headers: {len(actual['install_tree']['headers'])}",
            "- Unexpected installed headers: "
            f"{len(actual['install_tree']['unexpected_headers'])}",
            f"- Overall: {'PASS' if passed else 'FAIL'}",
            "",
            "## Interpretation",
            "",
            "A passing result proves an external embedded frontend can consume the",
            "installed package through the exported CMake config, include every",
            "public Photospider header from the package include root, and avoid",
            "repository-internal include roots or non-`photospider` headers.",
        ]
    )


def write_json(path: Path, payload: dict[str, Any]) -> None:
    """Write deterministic JSON evidence."""

    path.write_text(
        json.dumps(payload, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )


def main() -> int:
    """Run the install/export consumer smoke test."""

    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", required=True)
    parser.add_argument("--build", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--config", default="")
    parser.add_argument("--retain-artifacts", action="store_true")
    args = parser.parse_args()

    repo = Path(args.repo).resolve()
    build = Path(args.build).resolve()
    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)

    prefix = out / "install-prefix"
    consumer_src = out / "consumer-src"
    consumer_build = out / "consumer-build"
    shutil.rmtree(prefix, ignore_errors=True)
    shutil.rmtree(consumer_src, ignore_errors=True)
    shutil.rmtree(consumer_build, ignore_errors=True)
    compiled_public_headers = write_consumer_project(repo, consumer_src)

    build_product_command = ["cmake", "--build", str(build), "--target", "photospider"]
    if args.config:
        build_product_command.extend(["--config", args.config])
    build_product_code = run_command(
        build_product_command, repo, out / "build_photospider.log"
    )

    install_command = [
        "cmake",
        "--install",
        str(build),
        "--prefix",
        str(prefix),
    ]
    if args.config:
        install_command.extend(["--config", args.config])
    install_code = run_command(install_command, repo, out / "install.log")

    configure_command = [
        "cmake",
        "-S",
        str(consumer_src),
        "-B",
        str(consumer_build),
        f"-DCMAKE_PREFIX_PATH={prefix}",
    ]
    osx_architectures = cmake_cache_value(build, "CMAKE_OSX_ARCHITECTURES")
    if osx_architectures:
        configure_command.append(f"-DCMAKE_OSX_ARCHITECTURES={osx_architectures}")
    configure_code = run_command(
        configure_command, repo, out / "consumer_configure.log"
    )

    build_command = ["cmake", "--build", str(consumer_build)]
    if args.config:
        build_command.extend(["--config", args.config])
    build_code = run_command(build_command, repo, out / "consumer_build.log")

    executable = consumer_build / "photospider_consumer"
    if os.name == "nt":
        executable = consumer_build / "Debug" / "photospider_consumer.exe"
        if args.config and args.config != "Debug":
            executable = consumer_build / args.config / "photospider_consumer.exe"
    run_code = 127
    if executable.exists():
        run_code = run_command([str(executable)], repo, out / "consumer_run.log")
    else:
        (out / "consumer_run.log").write_text(
            f"consumer executable not found: {executable}\n",
            encoding="utf-8",
        )

    actual = {
        "commands": {
            "build_photospider": build_product_code,
            "install": install_code,
            "consumer_configure": configure_code,
            "consumer_build": build_code,
            "consumer_run": run_code,
        },
        "install_tree": inspect_install_tree(repo, prefix),
        "paths": {
            "repo": str(repo),
            "build": str(build),
            "prefix": str(prefix),
            "consumer_source": str(consumer_src),
            "consumer_build": str(consumer_build),
            "consumer_osx_architectures": osx_architectures,
            "transient_artifacts_retained": args.retain_artifacts,
        },
        "consumer": {
            "compiled_public_headers": compiled_public_headers,
        },
    }
    expected = {
        "consumer_target": "Photospider::photospider",
        "installed_archive": "lib/libphotospider.a",
        "allowed_header_root": "include/photospider",
        "unexpected_headers": [],
        "expected_command_exit_code": 0,
    }
    passed, compare = make_compare(actual)

    if not args.retain_artifacts:
        shutil.rmtree(prefix, ignore_errors=True)
        shutil.rmtree(consumer_src, ignore_errors=True)
        shutil.rmtree(consumer_build, ignore_errors=True)

    write_json(out / "expected.json", expected)
    write_json(out / "actual.json", actual)
    (out / "compare.log").write_text(compare, encoding="utf-8")
    (out / "summary.md").write_text(
        make_summary(out, actual, passed) + "\n", encoding="utf-8"
    )
    print(f"overall={'PASS' if passed else 'FAIL'}")
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
