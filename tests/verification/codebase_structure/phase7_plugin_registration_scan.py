#!/usr/bin/env python3
"""Verify issue #33 host-provided operation plugin registration boundaries."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path
from typing import Any


PLUGIN_SOURCE_ROOTS = (
    "custom_ops",
    "tests/fixtures/ops",
)
REQUIRED_DOCS = (
    "docs/kernel-architecture/Plugin-ABI.md",
    "docs/kernel-architecture/zh/Plugin-ABI.zh.md",
    "docs/codebase-structure/Codebase-Structure-Direction.md",
    "docs/codebase-structure/zh/Codebase-Structure-Direction.zh.md",
)
REQUIRED_DOC_TERMS = (
    "register_photospider_ops_v1",
    "OperationPluginRegistrar",
    "photospider_operation_plugin_shim",
)
SHIM_FORBIDDEN_SOURCES = (
    "src/ps_types.cpp",
    "src/ops.cpp",
    "src/kernel/plugin_manager.cpp",
    "src/kernel/plugin_manage_module/plugin_loader.cpp",
)
LEGACY_ENTRY_RE = re.compile(
    r'extern\s+"C"\s+PLUGIN_API\s+void\s+register_photospider_ops\s*'
    r"\(\s*\)"
)
V1_ENTRY_RE = re.compile(
    r'extern\s+"C"\s+PLUGIN_API\s+void\s+register_photospider_ops_v1\s*'
    r"\((?P<parameters>[^)]*)\)",
    re.DOTALL,
)
REGISTRAR_PARAMETER_RE = re.compile(
    r"(?:\bps\s*::\s*)?\bOperationPluginRegistrar\s*\*"
)


def strip_comments(text: str) -> str:
    """Return source text with C and C++ comments removed."""

    def replace_block(match: re.Match[str]) -> str:
        return "\n" * match.group(0).count("\n")

    without_blocks = re.sub(r"/\*.*?\*/", replace_block, text, flags=re.DOTALL)
    return "\n".join(
        line.split("//", 1)[0] for line in without_blocks.splitlines()
    )


def line_number_for_offset(text: str, offset: int) -> int:
    """Convert a string offset to a one-based line number."""

    return text.count("\n", 0, offset) + 1


def rel(repo: Path, path: Path) -> str:
    """Return a repository-relative path string."""

    return path.relative_to(repo).as_posix()


def operation_plugin_sources(repo: Path) -> list[Path]:
    """Return repo-owned operation plugin and fixture source files."""

    sources: list[Path] = []
    for root in PLUGIN_SOURCE_ROOTS:
        sources.extend((repo / root).glob("*.cpp"))
    return sorted(path for path in sources if path.is_file())


def v1_entry_has_registrar_parameter(match: re.Match[str]) -> bool:
    """Return true when the v1 entry signature receives the registrar pointer."""

    return REGISTRAR_PARAMETER_RE.search(match.group("parameters")) is not None


def has_v1_entry_with_registrar_parameter(code: str) -> bool:
    """Return true when any v1 entry signature has the registrar parameter."""

    return any(
        v1_entry_has_registrar_parameter(match)
        for match in V1_ENTRY_RE.finditer(code)
    )


def scan_plugin_sources(repo: Path) -> dict[str, Any]:
    """Inspect operation plugin sources for registrar-only registration."""

    registry_hits: list[dict[str, Any]] = []
    legacy_entry_hits: list[dict[str, Any]] = []
    missing_v1_entries: list[dict[str, Any]] = []
    missing_registrar_parameter: list[dict[str, Any]] = []
    source_rows: list[dict[str, Any]] = []

    registry_re = re.compile(r"\bOpRegistry\s*::\s*instance\s*\(")

    for path in operation_plugin_sources(repo):
        raw = path.read_text(encoding="utf-8")
        code = strip_comments(raw)
        source = rel(repo, path)
        v1_entry_matches = list(V1_ENTRY_RE.finditer(code))
        has_v1 = bool(v1_entry_matches)
        has_registrar_parameter = any(
            v1_entry_has_registrar_parameter(match)
            for match in v1_entry_matches
        )
        source_rows.append(
            {
                "file": source,
                "has_v1_entry": has_v1,
                "has_v1_registrar_parameter": has_registrar_parameter,
            }
        )
        if not has_v1:
            missing_v1_entries.append({"file": source})
        if not has_registrar_parameter:
            missing_registrar_parameter.append({"file": source})
        for match in registry_re.finditer(code):
            registry_hits.append(
                {
                    "file": source,
                    "line": line_number_for_offset(code, match.start()),
                    "text": " ".join(match.group(0).split()),
                }
            )
        for match in LEGACY_ENTRY_RE.finditer(code):
            legacy_entry_hits.append(
                {
                    "file": source,
                    "line": line_number_for_offset(code, match.start()),
                    "text": " ".join(match.group(0).split()),
                }
            )

    return {
        "sources": source_rows,
        "source_count": len(source_rows),
        "forbidden_registry_instance_hits": registry_hits,
        "legacy_no_arg_entry_hits": legacy_entry_hits,
        "missing_v1_entries": missing_v1_entries,
        "missing_registrar_parameter": missing_registrar_parameter,
    }


def first_target_link_block(cmake: str, target: str) -> str:
    """Return the first target_link_libraries block for a CMake target."""

    pattern = re.compile(
        rf"target_link_libraries\s*\(\s*{re.escape(target)}\b.*?\n\s*\)",
        re.DOTALL,
    )
    match = pattern.search(cmake)
    return "" if match is None else match.group(0)


def generic_operation_plugin_loop(cmake: str) -> str:
    """Return the CMake foreach block that configures custom_ops plugins."""

    start = cmake.find("foreach(PLUGIN_SOURCE ${PLUGIN_SOURCES})")
    if start < 0:
        return ""
    end = cmake.find("endforeach()", start)
    if end < 0:
        return ""
    return cmake[start : end + len("endforeach()")]


def add_library_block(cmake: str, target: str) -> str:
    """Return the first add_library block for a CMake target."""

    pattern = re.compile(
        rf"add_library\s*\(\s*{re.escape(target)}\b.*?\n\s*\)",
        re.DOTALL,
    )
    match = pattern.search(cmake)
    return "" if match is None else match.group(0)


def inspect_cmake(repo: Path) -> dict[str, Any]:
    """Check operation plugin targets avoid broad backend linking."""

    cmake = (repo / "CMakeLists.txt").read_text(encoding="utf-8")
    generic_loop = generic_operation_plugin_loop(cmake)
    lifecycle_block = first_target_link_block(cmake, "lifecycle_op_plugin")
    override_block = first_target_link_block(cmake, "override_lifecycle_op_plugin")
    metal_loader_block = first_target_link_block(cmake, "metal_ops_loader")
    metal_impl_block = first_target_link_block(cmake, "perlin_noise_metal")
    shim_block = add_library_block(cmake, "photospider_operation_plugin_shim")

    target_blocks = {
        "generic_custom_ops": generic_loop,
        "lifecycle_op_plugin": lifecycle_block,
        "override_lifecycle_op_plugin": override_block,
        "metal_ops_loader": metal_loader_block,
        "perlin_noise_metal": metal_impl_block,
    }
    broad_backend_hits = [
        {"target": target, "contains_photospider_lib": "photospider_lib" in block}
        for target, block in target_blocks.items()
        if "photospider_lib" in block
    ]
    return {
        "declares_operation_plugin_shim": bool(shim_block),
        "shim_uses_runtime_adapter_sources": (
            "src/image_buffer.cpp" in shim_block
            and "src/adapter/buffer_adapter_opencv.cpp" in shim_block
        ),
        "shim_forbidden_source_hits": [
            source for source in SHIM_FORBIDDEN_SOURCES if source in shim_block
        ],
        "generic_custom_ops_use_shim": (
            "photospider_operation_plugin_shim" in generic_loop
        ),
        "operation_plugin_targets_with_photospider_lib": broad_backend_hits,
        "phase7_scan_registered": "Phase7PluginRegistrationScan" in cmake,
    }


def inspect_loader_and_api(repo: Path) -> dict[str, Any]:
    """Check loader and public plugin API use the versioned registrar entry."""

    plugin_api = (repo / "include" / "plugin_api.hpp").read_text(
        encoding="utf-8"
    )
    loader = (
        repo / "src" / "kernel" / "plugin_manage_module" / "plugin_loader.cpp"
    ).read_text(encoding="utf-8")
    plugin_api_code = strip_comments(plugin_api)
    return {
        "plugin_api_declares_registrar": "OperationPluginRegistrar" in plugin_api,
        "plugin_api_declares_v1_symbol": "register_photospider_ops_v1" in plugin_api,
        "plugin_api_declares_old_no_arg_symbol": (
            re.search(
                r'extern\s+"C"\s+PLUGIN_API\s+void\s+register_photospider_ops\s*'
                r"\(\s*\)",
                plugin_api_code,
            )
            is not None
        ),
        "loader_resolves_v1_constant": "kOperationPluginRegisterSymbolV1" in loader,
        "loader_builds_host_registrar": (
            "make_operation_plugin_registrar" in loader
            and "HostRegistrarContext" in loader
        ),
    }


def inspect_docs(repo: Path) -> dict[str, Any]:
    """Check English docs and Chinese mirrors mention the new ABI boundary."""

    rows: list[dict[str, Any]] = []
    for path_text in REQUIRED_DOCS:
        path = repo / path_text
        text = path.read_text(encoding="utf-8") if path.is_file() else ""
        rows.append(
            {
                "file": path_text,
                "present": path.is_file(),
                "required_terms": [
                    {"term": term, "present": term in text}
                    for term in REQUIRED_DOC_TERMS
                ],
            }
        )
    return {"docs": rows}


def detector_selftest() -> dict[str, Any]:
    """Exercise source detectors against synthetic registration snippets."""

    legacy = 'extern "C" PLUGIN_API void register_photospider_ops() {}'
    current = (
        'extern "C" PLUGIN_API void register_photospider_ops_v1(\n'
        "    ps::OperationPluginRegistrar * registrar) {"
        "registrar->register_op_hp_monolithic(\"a\", \"b\", fn);}"
    )
    global_namespace = (
        'extern "C" PLUGIN_API void register_photospider_ops_v1(\n'
        "    OperationPluginRegistrar* registrar) {"
        "registrar->register_op_hp_monolithic(\"a\", \"b\", fn);}"
    )
    half_migrated = (
        'extern "C" PLUGIN_API void register_photospider_ops_v1() {'
        "OperationPluginRegistrar* registrar = nullptr;"
        "}"
    )
    commented = "// ps::OpRegistry::instance().register_op_hp_monolithic();"
    return {
        "detects_legacy_no_arg_entry": re.search(
            LEGACY_ENTRY_RE,
            strip_comments(legacy),
        )
        is not None,
        "detects_v1_registrar_parameter": has_v1_entry_with_registrar_parameter(
            strip_comments(current)
        ),
        "detects_v1_global_registrar_parameter": (
            has_v1_entry_with_registrar_parameter(strip_comments(global_namespace))
        ),
        "rejects_v1_without_signature_registrar_parameter": (
            not has_v1_entry_with_registrar_parameter(strip_comments(half_migrated))
        ),
        "ignores_commented_registry_singleton": "OpRegistry::instance"
        not in strip_comments(commented),
    }


def write_json(path: Path, payload: dict[str, Any]) -> None:
    """Write deterministic JSON output."""

    path.write_text(
        json.dumps(payload, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )


def make_expected() -> dict[str, Any]:
    """Return the expected phase-7 guardrail state."""

    return {
        "plugin_sources": {
            "source_count_minimum": 6,
            "forbidden_registry_instance_hits": [],
            "legacy_no_arg_entry_hits": [],
            "missing_v1_entries": [],
            "missing_registrar_parameter": [],
        },
        "cmake": {
            "declares_operation_plugin_shim": True,
            "shim_uses_runtime_adapter_sources": True,
            "shim_forbidden_source_hits": [],
            "generic_custom_ops_use_shim": True,
            "operation_plugin_targets_with_photospider_lib": [],
            "phase7_scan_registered": True,
        },
        "loader_and_api": {
            "plugin_api_declares_registrar": True,
            "plugin_api_declares_v1_symbol": True,
            "plugin_api_declares_old_no_arg_symbol": False,
            "loader_resolves_v1_constant": True,
            "loader_builds_host_registrar": True,
        },
        "docs": {"all_required_terms_present": True},
        "selftest": {
            "detects_legacy_no_arg_entry": True,
            "detects_v1_registrar_parameter": True,
            "detects_v1_global_registrar_parameter": True,
            "rejects_v1_without_signature_registrar_parameter": True,
            "ignores_commented_registry_singleton": True,
        },
    }


def make_compare(actual: dict[str, Any], expected: dict[str, Any]) -> tuple[bool, str]:
    """Create PASS/FAIL comparison text for issue #33 guardrails."""

    docs_all_terms_present = all(
        row["present"]
        and all(term["present"] for term in row["required_terms"])
        for row in actual["docs"]["docs"]
    )
    checks = {
        "operation plugin source inventory is complete": actual["plugin_sources"][
            "source_count"
        ]
        >= expected["plugin_sources"]["source_count_minimum"],
        "plugins do not call OpRegistry::instance in code": actual[
            "plugin_sources"
        ]["forbidden_registry_instance_hits"]
        == expected["plugin_sources"]["forbidden_registry_instance_hits"],
        "plugins no longer export the no-arg entry": actual["plugin_sources"][
            "legacy_no_arg_entry_hits"
        ]
        == expected["plugin_sources"]["legacy_no_arg_entry_hits"],
        "plugins export register_photospider_ops_v1": actual["plugin_sources"][
            "missing_v1_entries"
        ]
        == expected["plugin_sources"]["missing_v1_entries"],
        "plugins receive OperationPluginRegistrar pointer in v1 signature": actual[
            "plugin_sources"
        ]["missing_registrar_parameter"]
        == expected["plugin_sources"]["missing_registrar_parameter"],
        "CMake declares the operation plugin shim": actual["cmake"][
            "declares_operation_plugin_shim"
        ]
        == expected["cmake"]["declares_operation_plugin_shim"],
        "shim carries runtime adapter sources only": (
            actual["cmake"]["shim_uses_runtime_adapter_sources"]
            == expected["cmake"]["shim_uses_runtime_adapter_sources"]
            and actual["cmake"]["shim_forbidden_source_hits"]
            == expected["cmake"]["shim_forbidden_source_hits"]
        ),
        "custom operation plugins link the shim": actual["cmake"][
            "generic_custom_ops_use_shim"
        ]
        == expected["cmake"]["generic_custom_ops_use_shim"],
        "operation plugin targets avoid photospider_lib": actual["cmake"][
            "operation_plugin_targets_with_photospider_lib"
        ]
        == expected["cmake"]["operation_plugin_targets_with_photospider_lib"],
        "phase-7 scan is registered with CTest": actual["cmake"][
            "phase7_scan_registered"
        ]
        == expected["cmake"]["phase7_scan_registered"],
        "plugin API exposes registrar and rejects old ABI": actual[
            "loader_and_api"
        ]
        == expected["loader_and_api"],
        "English docs and Chinese mirrors mention new boundary": docs_all_terms_present
        == expected["docs"]["all_required_terms_present"],
        "detectors self-test": actual["selftest"] == expected["selftest"],
    }

    lines = [
        f"{'PASS' if passed else 'FAIL'}: {label}"
        for label, passed in checks.items()
    ]
    return all(checks.values()), "\n".join(lines) + "\n"


def write_summary(path: Path, passed: bool, actual: dict[str, Any]) -> None:
    """Write a reader-facing summary for the phase-7 scan."""

    status = "PASS" if passed else "FAIL"
    docs = actual["docs"]["docs"]
    target_hits = actual["cmake"]["operation_plugin_targets_with_photospider_lib"]
    body = [
        "# Phase 7 Plugin Registration Scan",
        "",
        f"Status: {status}",
        "",
        "This scan verifies issue #33 guardrails for host-provided operation "
        "plugin registration.",
        "",
        f"- Operation plugin sources scanned: {actual['plugin_sources']['source_count']}",
        "- Direct `OpRegistry::instance()` code hits: "
        f"{len(actual['plugin_sources']['forbidden_registry_instance_hits'])}",
        "- Legacy no-argument entry hits: "
        f"{len(actual['plugin_sources']['legacy_no_arg_entry_hits'])}",
        "- Operation plugin targets still linking `photospider_lib`: "
        f"{len(target_hits)}",
        "- Required docs checked: " + ", ".join(row["file"] for row in docs),
        "",
        "See `actual.json` for raw observations and `compare.log` for each "
        "guardrail result.",
        "",
    ]
    path.write_text("\n".join(body), encoding="utf-8")


def main() -> int:
    """Run the phase-7 plugin registration scan."""

    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()

    repo = args.repo.resolve()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)

    actual = {
        "plugin_sources": scan_plugin_sources(repo),
        "cmake": inspect_cmake(repo),
        "loader_and_api": inspect_loader_and_api(repo),
        "docs": inspect_docs(repo),
        "selftest": detector_selftest(),
    }
    expected = make_expected()
    passed, compare = make_compare(actual, expected)

    write_json(out / "actual.json", actual)
    write_json(out / "expected.json", expected)
    (out / "compare.log").write_text(compare, encoding="utf-8")
    write_summary(out / "scan-summary.md", passed, actual)

    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
