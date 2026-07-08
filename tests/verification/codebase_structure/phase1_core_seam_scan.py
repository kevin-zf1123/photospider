#!/usr/bin/env python3
"""Verify the phase-1 include/photospider/core value-type seam."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path
from typing import Any


CORE_HEADERS = (
    "include/photospider/core/compute_intent.hpp",
    "include/photospider/core/export.hpp",
    "include/photospider/core/geometry.hpp",
    "include/photospider/core/graph_error.hpp",
    "include/photospider/core/image_buffer.hpp",
    "include/photospider/core/inspection_types.hpp",
    "include/photospider/core/result_types.hpp",
)
VALUE_CONTRACT_HEADERS = tuple(
    path for path in CORE_HEADERS if path != "include/photospider/core/export.hpp"
)
LEGACY_HEADERS = (
    "include/image_buffer.hpp",
    "include/ps_types.hpp",
)
FORBIDDEN_INCLUDE_PREFIXES = (
    "src/",
    "kernel/services/",
)
FORBIDDEN_CORE_EXTERNAL_INCLUDES = (
    "opencv2/",
    "yaml-cpp/",
)
IMPLEMENTATION_ONLY_TYPES = (
    "GraphModel",
    "GraphRuntime",
    "GraphStateExecutor",
    "ComputeService",
    "DirtyControlLane",
    "ComputePlan",
    "FullTaskGraph",
    "CpuWorkStealingScheduler",
    "GpuPipelineScheduler",
    "SerialDebugScheduler",
    "GraphCacheService",
    "GraphTraversalService",
    "GraphIoService",
    "GraphInspectService",
    "RoiPropagationService",
)
REQUIRED_PUBLIC_TYPES = (
    "ComputeIntent",
    "GraphErrc",
    "GraphError",
    "DataType",
    "Device",
    "ImageBuffer",
    "PixelRect",
    "PixelSize",
    "InputTileView",
    "OutputTileView",
    "OperationStatus",
    "Result",
    "VoidResult",
    "GraphSessionId",
    "NodeId",
    "NodeInspectionView",
    "GraphInspectionView",
    "DirtyRegionInspectionSnapshot",
    "SchedulerStatusSnapshot",
)
INCLUDE_RE = re.compile(r"^\s*#\s*include\s*[<\"]([^>\"]+)[>\"]")
NAMESPACE_PS_RE = re.compile(r"\bnamespace\s+ps\b")


def rel(repo: Path, path: Path) -> str:
    return path.relative_to(repo).as_posix()


def strip_comments(text: str) -> str:
    def replace_block(match: re.Match[str]) -> str:
        return "\n" * match.group(0).count("\n")

    without_blocks = re.sub(r"/\*.*?\*/", replace_block, text, flags=re.DOTALL)
    return "\n".join(line.split("//", 1)[0] for line in without_blocks.splitlines())


def public_headers(repo: Path) -> list[Path]:
    root = repo / "include" / "photospider"
    headers: list[Path] = []
    for pattern in ("*.h", "*.hpp", "*.hh", "*.hxx"):
        headers.extend(root.rglob(pattern))
    return sorted(path for path in headers if path.is_file())


def scan_public_headers(repo: Path) -> dict[str, Any]:
    include_violations: list[dict[str, Any]] = []
    core_external_include_violations: list[dict[str, Any]] = []
    implementation_type_occurrences: list[dict[str, Any]] = []
    headers = public_headers(repo)
    for path in headers:
        header = rel(repo, path)
        text = path.read_text(encoding="utf-8")
        code_text = strip_comments(text)
        for line_number, line in enumerate(code_text.splitlines(), start=1):
            match = INCLUDE_RE.match(line)
            if match:
                include = match.group(1).replace("\\", "/")
                for prefix in FORBIDDEN_INCLUDE_PREFIXES:
                    if include == prefix.rstrip("/") or include.startswith(prefix):
                        include_violations.append(
                            {
                                "file": header,
                                "line": line_number,
                                "include": include,
                                "reason": f"forbidden include prefix `{prefix}`",
                            }
                        )
                if "/src/" in include or include.startswith("../src/"):
                    include_violations.append(
                        {
                            "file": header,
                            "line": line_number,
                            "include": include,
                            "reason": "forbidden implementation source include",
                        }
                    )
                if header.startswith("include/photospider/core/"):
                    for prefix in FORBIDDEN_CORE_EXTERNAL_INCLUDES:
                        if include.startswith(prefix):
                            core_external_include_violations.append(
                                {
                                    "file": header,
                                    "line": line_number,
                                    "include": include,
                                    "reason": (
                                        "forbidden core external include "
                                        f"`{prefix}`"
                                    ),
                                }
                            )
            for type_name in IMPLEMENTATION_ONLY_TYPES:
                if re.search(rf"\b{re.escape(type_name)}\b", line):
                    implementation_type_occurrences.append(
                        {
                            "file": header,
                            "line": line_number,
                            "type": type_name,
                            "text": line.strip(),
                        }
                    )
    return {
        "headers": [rel(repo, path) for path in headers],
        "header_count": len(headers),
        "include_violations": include_violations,
        "core_external_include_violations": core_external_include_violations,
        "implementation_type_occurrences": implementation_type_occurrences,
    }


def inspect_core_headers(repo: Path) -> dict[str, Any]:
    rows = []
    combined_text = ""
    for path_name in CORE_HEADERS:
        path = repo / path_name
        text = path.read_text(encoding="utf-8") if path.exists() else ""
        code_text = strip_comments(text)
        combined_text += "\n" + code_text
        rows.append(
            {
                "path": path_name,
                "exists": path.exists(),
                "uses_namespace_ps": (
                    path_name not in VALUE_CONTRACT_HEADERS
                    or bool(NAMESPACE_PS_RE.search(code_text))
                ),
                "has_doxygen_file_brief": "@file" in text and "@brief" in text,
            }
        )
    type_rows = []
    for type_name in REQUIRED_PUBLIC_TYPES:
        type_decl_re = re.compile(
            rf"\b(class|struct|enum\s+class)\s+"
            rf"(?:[A-Z_][A-Z0-9_]*\s+)?{re.escape(type_name)}\b"
        )
        type_rows.append(
            {
                "name": type_name,
                "declared": bool(type_decl_re.search(combined_text)),
            }
        )
    return {
        "headers": rows,
        "required_types": type_rows,
    }


def inspect_legacy_headers(repo: Path) -> dict[str, Any]:
    rows = []
    for path_name in LEGACY_HEADERS:
        path = repo / path_name
        text = path.read_text(encoding="utf-8") if path.exists() else ""
        rows.append(
            {
                "path": path_name,
                "exists": path.exists(),
                "includes_photospider_core": "photospider/core/" in text,
            }
        )
    return {"headers": rows}


def write_json(path: Path, payload: dict[str, Any]) -> None:
    path.write_text(
        json.dumps(payload, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )


def make_summary(out: Path, actual: dict[str, Any], passed: bool) -> str:
    public_headers = actual["public_headers"]
    return "\n".join(
        [
            "# codebase-refactor phase-1 core seam evidence",
            "",
            "## Test objective",
            "",
            "Verify issue #28 introduced stable value contracts under",
            "`include/photospider/core` while keeping legacy internal include",
            "paths buildable and the installable public header boundary clean.",
            "",
            "## Evidence files",
            "",
            f"- `expected.json`: `{out / 'expected.json'}`",
            f"- `actual.json`: `{out / 'actual.json'}`",
            f"- `compare.log`: `{out / 'compare.log'}`",
            "",
            "## Result",
            "",
            f"- Public headers scanned: {public_headers['header_count']}",
            f"- Core headers expected: {len(CORE_HEADERS)}",
            f"- Public include violations: {len(public_headers['include_violations'])}",
            "- Core external include violations: "
            f"{len(public_headers['core_external_include_violations'])}",
            "- Implementation-only type occurrences: "
            f"{len(public_headers['implementation_type_occurrences'])}",
            f"- Overall: {'PASS' if passed else 'FAIL'}",
            "",
            "## Interpretation",
            "",
            "The scan proves the phase-1 public core seam exists, declares the",
            "required namespace `ps` value contracts, keeps legacy include paths",
            "wired through the new core headers, and does not expose forbidden",
            "`src/`, `kernel/services/...`, GraphModel, GraphRuntime, or",
            "ComputeService implementation details through `include/photospider`.",
            "It also proves core value headers do not require OpenCV or yaml-cpp",
            "includes.",
            "",
            "## Replay command logs",
            "",
            "- Commands: `commands.log`",
            "- CMake configure: `cmake_configure.log`",
            "- Public header self-containment build: "
            "`public_header_self_containment_build.log`",
            "- Public header CTest: `public_header_ctest.log`",
            "- Full build: `build_all.log`",
            "- graph_cli build: `graph_cli_build.log`",
            "- Python compile check: `py_compile.log`",
            "- Primary repo diff check: `git_diff_check.log`",
            "- Personal overlay diff check: `git_personal_diff_check.log`",
        ]
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", required=True)
    parser.add_argument("--out", required=True)
    args = parser.parse_args()

    repo = Path(args.repo).resolve()
    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)

    actual = {
        "core_headers": inspect_core_headers(repo),
        "legacy_headers": inspect_legacy_headers(repo),
        "public_headers": scan_public_headers(repo),
    }
    expected = {
        "core_headers": {
            "all_exist": True,
            "all_use_namespace_ps": True,
            "all_have_doxygen_file_brief": True,
            "all_required_types_declared": True,
        },
        "legacy_headers": {
            "all_exist": True,
            "all_include_photospider_core": True,
        },
        "public_headers": {
            "include_violations": [],
            "core_external_include_violations": [],
            "implementation_type_occurrences": [],
        },
    }
    checks = [
        (
            "core_headers.all_exist",
            all(row["exists"] for row in actual["core_headers"]["headers"])
            == expected["core_headers"]["all_exist"],
        ),
        (
            "core_headers.all_use_namespace_ps",
            all(row["uses_namespace_ps"] for row in actual["core_headers"]["headers"])
            == expected["core_headers"]["all_use_namespace_ps"],
        ),
        (
            "core_headers.all_have_doxygen_file_brief",
            all(row["has_doxygen_file_brief"] for row in actual["core_headers"]["headers"])
            == expected["core_headers"]["all_have_doxygen_file_brief"],
        ),
        (
            "core_headers.all_required_types_declared",
            all(row["declared"] for row in actual["core_headers"]["required_types"])
            == expected["core_headers"]["all_required_types_declared"],
        ),
        (
            "legacy_headers.all_exist",
            all(row["exists"] for row in actual["legacy_headers"]["headers"])
            == expected["legacy_headers"]["all_exist"],
        ),
        (
            "legacy_headers.all_include_photospider_core",
            all(
                row["includes_photospider_core"]
                for row in actual["legacy_headers"]["headers"]
            )
            == expected["legacy_headers"]["all_include_photospider_core"],
        ),
        (
            "public_headers.include_violations",
            actual["public_headers"]["include_violations"]
            == expected["public_headers"]["include_violations"],
        ),
        (
            "public_headers.core_external_include_violations",
            actual["public_headers"]["core_external_include_violations"]
            == expected["public_headers"]["core_external_include_violations"],
        ),
        (
            "public_headers.implementation_type_occurrences",
            actual["public_headers"]["implementation_type_occurrences"]
            == expected["public_headers"]["implementation_type_occurrences"],
        ),
    ]
    passed = all(ok for _, ok in checks)

    write_json(out / "expected.json", expected)
    write_json(out / "actual.json", actual)
    (out / "compare.log").write_text(
        "\n".join(f"{name}: {'PASS' if ok else 'FAIL'}" for name, ok in checks)
        + f"\noverall={'PASS' if passed else 'FAIL'}\n",
        encoding="utf-8",
    )
    (out / "summary.md").write_text(
        make_summary(out, actual, passed) + "\n", encoding="utf-8"
    )
    print(f"overall={'PASS' if passed else 'FAIL'}")
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
