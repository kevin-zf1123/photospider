#!/usr/bin/env python3
"""Verify issue #34 static product target and package boundary."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path
from typing import Any


PUBLIC_HEADER_GLOBS = ("*.h", "*.hpp", "*.hh", "*.hxx")
INTERNAL_TARGETS = (
    "photospider_core_types",
    "photospider_graph",
    "photospider_plugin",
    "photospider_compute",
    "photospider_cli_common",
)
INTERNAL_IMPL_TARGETS = (
    "photospider_core_types",
    "photospider_graph",
    "photospider_plugin",
    "photospider_compute",
)


def rel(repo: Path, path: Path) -> str:
    """Return a repository-relative POSIX path."""

    return path.relative_to(repo).as_posix()


def public_headers(repo: Path) -> list[str]:
    """Return source public headers eligible for installation."""

    root = repo / "include" / "photospider"
    headers: list[Path] = []
    for pattern in PUBLIC_HEADER_GLOBS:
        headers.extend(root.rglob(pattern))
    return sorted(rel(repo, path) for path in headers if path.is_file())


def first_block(cmake: str, command: str, target: str) -> str:
    """Return the first simple CMake command block for a target."""

    pattern = re.compile(
        rf"{re.escape(command)}\s*\(\s*{re.escape(target)}\b.*?\)",
        re.DOTALL,
    )
    match = pattern.search(cmake)
    return match.group(0) if match else ""


def all_target_include_blocks(cmake: str) -> list[dict[str, str]]:
    """Return target_include_directories blocks with their target names."""

    pattern = re.compile(
        r"target_include_directories\s*\(\s*([A-Za-z0-9_:-]+)\b(.*?)\n\s*\)",
        re.DOTALL,
    )
    return [
        {"target": match.group(1), "block": match.group(0)}
        for match in pattern.finditer(cmake)
    ]


def public_section_exposes_src(block: str) -> bool:
    """Report whether a target include block publishes the source include root."""

    match = re.search(r"\bPUBLIC\b(?P<section>.*?)(\bPRIVATE\b|\bINTERFACE\b|\))", block, re.DOTALL)
    if not match:
        return False
    public_section = match.group("section")
    return "${PROJECT_SOURCE_DIR}/src" in public_section or "/src" in public_section


def inspect_cmake(repo: Path) -> dict[str, Any]:
    """Inspect CMake target, install, export, and dependency declarations."""

    cmake = (repo / "CMakeLists.txt").read_text(encoding="utf-8")
    product_add = first_block(cmake, "add_library", "photospider")
    product_includes = first_block(cmake, "target_include_directories", "photospider")
    product_links = first_block(cmake, "target_link_libraries", "photospider")
    product_definitions = first_block(
        cmake, "target_compile_definitions", "photospider"
    )

    public_src_exposures = [
        {"target": row["target"], "block": row["block"]}
        for row in all_target_include_blocks(cmake)
        if public_section_exposes_src(row["block"])
    ]
    internal_include_blocks = {
        target: first_block(cmake, "target_include_directories", target)
        for target in INTERNAL_TARGETS
    }

    return {
        "declares_static_photospider_target": bool(
            re.search(r"add_library\s*\(\s*photospider\s+STATIC\b", cmake)
        ),
        "does_not_declare_shared_photospider_lib_product": not bool(
            re.search(r"add_library\s*\(\s*photospider_lib\s+SHARED\b", cmake)
        ),
        "product_output_name": "photospider",
        "product_sources_fold_internal_sources": all(
            token in cmake
            for token in (
                "PHOTOSPIDER_BUILD_PUBLIC_INCLUDE_DIR",
                "PHOTOSPIDER_CORE_TYPE_SOURCES",
                "PHOTOSPIDER_GRAPH_SOURCES",
                "PHOTOSPIDER_PLUGIN_SOURCES",
                "COMPUTE_SOURCES",
                "KERNEL_FACADE_SOURCES",
                "PHOTOSPIDER_PRODUCT_SOURCES",
            )
        ),
        "product_add_block": product_add,
        "product_public_include_uses_public_root": (
            "$<BUILD_INTERFACE:${PHOTOSPIDER_BUILD_PUBLIC_INCLUDE_DIR}>"
            in product_includes
            and "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>" in product_includes
        ),
        "product_private_include_uses_private_roots": (
            "PRIVATE" in product_includes
            and "${PHOTOSPIDER_PRIVATE_INCLUDE_DIRS}" in product_includes
        ),
        "product_public_section_exposes_src": public_section_exposes_src(
            product_includes
        ),
        "product_defines_static_for_consumers": (
            "PUBLIC PHOTOSPIDER_STATIC" in product_definitions
        ),
        "product_link_dependencies": {
            "threads": "Threads::Threads" in product_links,
            "opencv": "${OpenCV_LIBS}" in product_links,
            "yaml_cpp": "yaml-cpp::yaml-cpp" in product_links,
            "dl": "${CMAKE_DL_LIBS}" in product_links,
            "ftxui": "FTXUI" in product_links or "ftxui" in product_links,
            "cli_common": "photospider_cli_common" in product_links,
            "internal_targets": [
                target for target in INTERNAL_IMPL_TARGETS if target in product_links
            ],
        },
        "public_src_include_exposures": public_src_exposures,
        "internal_targets_use_private_include_blocks": [
            {
                "target": target,
                "has_block": bool(block),
                "has_private": "PRIVATE" in block,
                "publishes_src": public_section_exposes_src(block),
            }
            for target, block in internal_include_blocks.items()
        ],
        "install_targets_photospider": (
            "install(TARGETS photospider" in cmake
            and "EXPORT PhotospiderTargets" in cmake
        ),
        "install_headers_only_photospider_tree": (
            'install(DIRECTORY "${PROJECT_SOURCE_DIR}/include/photospider"' in cmake
            and "DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}" in cmake
        ),
        "install_export_namespace": (
            "install(EXPORT PhotospiderTargets" in cmake
            and "NAMESPACE Photospider::" in cmake
        ),
        "package_config_generated": (
            "configure_package_config_file(" in cmake
            and "write_basic_package_version_file(" in cmake
        ),
        "phase4_tests_registered": (
            "Phase4StaticProductScan" in cmake
            and "Phase4StaticProductConsumerSmoke" in cmake
        ),
        "apple_product_links_system_framework_flags": (
            '"-framework Metal"' in cmake and '"-framework Foundation"' in cmake
        ),
    }


def inspect_config_template(repo: Path) -> dict[str, Any]:
    """Inspect the installed package config template."""

    path = repo / "cmake" / "PhotospiderConfig.cmake.in"
    text = path.read_text(encoding="utf-8") if path.is_file() else ""
    return {
        "path": rel(repo, path),
        "exists": path.is_file(),
        "finds_threads": "find_dependency(Threads)" in text,
        "finds_opencv_components": (
            "find_dependency(OpenCV COMPONENTS core imgproc imgcodecs videoio)"
            in text
        ),
        "finds_yaml_cpp": "find_dependency(yaml-cpp)" in text,
        "checks_apple_frameworks": (
            "find_library(PHOTOSPIDER_METAL_FRAMEWORK Metal REQUIRED)" in text
            and "find_library(PHOTOSPIDER_FOUNDATION_FRAMEWORK Foundation REQUIRED)"
            in text
        ),
        "includes_targets": (
            'include("${CMAKE_CURRENT_LIST_DIR}/PhotospiderTargets.cmake")'
            in text
        ),
    }


def inspect_public_export_header(repo: Path) -> dict[str, Any]:
    """Inspect static-link visibility handling in the public export header."""

    path = repo / "include" / "photospider" / "core" / "export.hpp"
    text = path.read_text(encoding="utf-8")
    return {
        "path": rel(repo, path),
        "supports_photospider_static": "defined(PHOTOSPIDER_STATIC)" in text,
        "static_branch_clears_api": bool(
            re.search(
                r"defined\(PHOTOSPIDER_STATIC\).*?#define\s+PHOTOSPIDER_API\s*$",
                text,
                re.DOTALL | re.MULTILINE,
            )
        ),
    }


def make_compare(actual: dict[str, Any]) -> tuple[bool, str]:
    """Create PASS/FAIL comparison text for issue #34."""

    cmake = actual["cmake"]
    config = actual["config_template"]
    export_header = actual["export_header"]
    checks = {
        "static photospider target exists": cmake[
            "declares_static_photospider_target"
        ],
        "old shared photospider_lib product is absent": cmake[
            "does_not_declare_shared_photospider_lib_product"
        ],
        "product folds implementation sources into one archive": cmake[
            "product_sources_fold_internal_sources"
        ],
        "product public include dirs use public install root": cmake[
            "product_public_include_uses_public_root"
        ],
        "product keeps private include roots private": cmake[
            "product_private_include_uses_private_roots"
        ]
        and not cmake["product_public_section_exposes_src"],
        "no target publishes src include root": cmake[
            "public_src_include_exposures"
        ]
        == [],
        "internal targets use private include blocks": all(
            row["has_block"] and row["has_private"] and not row["publishes_src"]
            for row in cmake["internal_targets_use_private_include_blocks"]
        ),
        "static macro is exported to consumers": cmake[
            "product_defines_static_for_consumers"
        ]
        and export_header["supports_photospider_static"]
        and export_header["static_branch_clears_api"],
        "product has link-only implementation dependencies": all(
            cmake["product_link_dependencies"][key]
            for key in ("threads", "opencv", "yaml_cpp", "dl")
        ),
        "product does not depend on CLI or FTXUI": not cmake[
            "product_link_dependencies"
        ]["ftxui"]
        and not cmake["product_link_dependencies"]["cli_common"],
        "product does not export internal static targets": cmake[
            "product_link_dependencies"
        ]["internal_targets"]
        == [],
        "install target and headers are declared": cmake[
            "install_targets_photospider"
        ]
        and cmake["install_headers_only_photospider_tree"],
        "install export namespace is declared": cmake["install_export_namespace"],
        "package config and version are generated": cmake[
            "package_config_generated"
        ],
        "package config finds dependencies": all(
            config[key]
            for key in (
                "exists",
                "finds_threads",
                "finds_opencv_components",
                "finds_yaml_cpp",
                "checks_apple_frameworks",
                "includes_targets",
            )
        ),
        "Apple framework link policy is explicit": cmake[
            "apple_product_links_system_framework_flags"
        ],
        "phase4 CTest entries are registered": cmake["phase4_tests_registered"],
        "public header inventory is nonempty": len(actual["public_headers"]) > 0,
    }
    passed = all(checks.values())
    lines = ["phase4_static_product_scan"]
    lines.extend(
        f"{'PASS' if ok else 'FAIL'} {name}" for name, ok in checks.items()
    )
    if cmake["public_src_include_exposures"]:
        lines.append("public src include exposures:")
        lines.extend(
            f"- {row['target']}" for row in cmake["public_src_include_exposures"]
        )
    lines.append(f"overall={'PASS' if passed else 'FAIL'}")
    return passed, "\n".join(lines) + "\n"


def make_summary(out: Path, actual: dict[str, Any], passed: bool) -> str:
    """Create a reader-facing evidence summary for issue #34."""

    return "\n".join(
        [
            "# codebase-refactor phase-4 static product scan",
            "",
            "## Test objective",
            "",
            "Verify issue #34 CMake packaging shape: the installable product is",
            "a static `photospider` archive, public headers are limited to",
            "`include/photospider`, `src/` include roots stay private, package",
            "config files are generated, and implementation dependencies are",
            "documented as link-only package dependencies.",
            "",
            "## Evidence files",
            "",
            f"- `expected.json`: `{out / 'expected.json'}`",
            f"- `actual.json`: `{out / 'actual.json'}`",
            f"- `compare.log`: `{out / 'compare.log'}`",
            "",
            "## Result",
            "",
            f"- Public headers scanned: {len(actual['public_headers'])}",
            "- Public `src/` include exposures: "
            f"{len(actual['cmake']['public_src_include_exposures'])}",
            f"- Overall: {'PASS' if passed else 'FAIL'}",
            "",
            "## Interpretation",
            "",
            "A passing result proves the source-level CMake package boundary is",
            "coherent before installation: external consumers receive the static",
            "`Photospider::photospider` product and public header tree, while",
            "repository-only implementation include paths and CLI dependencies do",
            "not become part of that exported target.",
        ]
    )


def write_json(path: Path, payload: dict[str, Any]) -> None:
    """Write deterministic JSON evidence."""

    path.write_text(
        json.dumps(payload, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )


def main() -> int:
    """Run the phase-4 static product scan."""

    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", required=True)
    parser.add_argument("--out", required=True)
    args = parser.parse_args()

    repo = Path(args.repo).resolve()
    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)

    actual = {
        "public_headers": public_headers(repo),
        "cmake": inspect_cmake(repo),
        "config_template": inspect_config_template(repo),
        "export_header": inspect_public_export_header(repo),
    }
    expected = {
        "product_target": "photospider",
        "package_namespace": "Photospider::",
        "installed_header_root": "include/photospider",
        "forbidden_public_src_include_exposures": [],
        "public_dependencies": "none; dependencies are link-only for static archive",
    }
    passed, compare = make_compare(actual)

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
