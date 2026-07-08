#!/usr/bin/env python3
"""Verify the phase-2 include/photospider/host adapter seam."""

from __future__ import annotations

import argparse
import json
import re
import shlex
import shutil
import subprocess
from pathlib import Path
from typing import Any


HOST_HEADERS = (
    "include/photospider/host/graph_session.hpp",
    "include/photospider/host/compute_request.hpp",
    "include/photospider/host/event_stream.hpp",
    "include/photospider/host/host.hpp",
)
HOST_IMPLEMENTATION = "src/host/embedded_host.cpp"
HOST_TEST = "tests/test_host_adapter.cpp"
REQUIRED_PUBLIC_TYPES = (
    "Host",
    "GraphLoadRequest",
    "HostComputeRequest",
    "ComputeEventSnapshot",
    "HostSchedulerTraceAction",
    "SchedulerInfoSnapshot",
    "HostDependencyTreeSnapshot",
    "HostPluginLoadReport",
)
REQUIRED_PUBLIC_FUNCTIONS = ("create_embedded_host",)
FORBIDDEN_HOST_INCLUDES = (
    "kernel/",
    "graph_model.hpp",
    "node.hpp",
    "ps_types.hpp",
    "opencv2/",
    "yaml-cpp/",
)
IMPLEMENTATION_ONLY_TYPES = (
    "Kernel",
    "GraphModel",
    "GraphRuntime",
    "GraphStateExecutor",
    "ComputeService",
    "InteractionService",
    "GraphInspectService",
    "GraphTraversalService",
    "GraphCacheService",
    "GraphEventService",
    "IScheduler",
)
INCLUDE_RE = re.compile(r"^\s*#\s*include\s*[<\"]([^>\"]+)[>\"]")
NAMESPACE_OPEN_RE_TEMPLATE = r"\bnamespace\s+{}\s*\{{"
TYPE_DECL_RE_TEMPLATE = (
    r"\b(class|struct|enum\s+class)\s+"
    r"(?:[A-Z_][A-Z0-9_]*\s+)?{}\b"
)


def rel(repo: Path, path: Path) -> str:
    try:
        return path.relative_to(repo).as_posix()
    except ValueError:
        return path.as_posix()


def strip_comments(text: str) -> str:
    def replace_block(match: re.Match[str]) -> str:
        return "\n" * match.group(0).count("\n")

    without_blocks = re.sub(r"/\*.*?\*/", replace_block, text, flags=re.DOTALL)
    return "\n".join(line.split("//", 1)[0] for line in without_blocks.splitlines())


def namespace_body_ranges(text: str, namespace_name: str) -> list[tuple[int, int]]:
    ranges: list[tuple[int, int]] = []
    namespace_re = re.compile(
        NAMESPACE_OPEN_RE_TEMPLATE.format(re.escape(namespace_name))
    )
    for match in namespace_re.finditer(text):
        open_brace = match.end() - 1
        depth = 1
        index = open_brace + 1
        while index < len(text):
            char = text[index]
            if char == "{":
                depth += 1
            elif char == "}":
                depth -= 1
                if depth == 0:
                    ranges.append((open_brace + 1, index))
                    break
            index += 1
    return ranges


def namespace_bodies(text: str, namespace_name: str) -> list[str]:
    return [
        text[start:end]
        for start, end in namespace_body_ranges(text, namespace_name)
    ]


def type_declared_in_namespace(
    code_text: str, type_name: str, namespace_name: str
) -> bool:
    type_decl_re = re.compile(TYPE_DECL_RE_TEMPLATE.format(re.escape(type_name)))
    return any(
        type_decl_re.search(body)
        for body in namespace_bodies(code_text, namespace_name)
    )


def symbol_declared_in_namespace(
    code_text: str, symbol_name: str, namespace_name: str
) -> bool:
    symbol_re = re.compile(rf"\b{re.escape(symbol_name)}\s*\(")
    return any(
        symbol_re.search(body)
        for body in namespace_bodies(code_text, namespace_name)
    )


def inspect_host_headers(repo: Path) -> dict[str, Any]:
    rows = []
    combined_code = ""
    include_violations = []
    implementation_type_occurrences = []
    for header in HOST_HEADERS:
        path = repo / header
        text = path.read_text(encoding="utf-8") if path.exists() else ""
        code_text = strip_comments(text)
        combined_code += "\n" + code_text
        rows.append(
            {
                "path": header,
                "exists": path.exists(),
                "uses_namespace_ps": bool(re.search(r"\bnamespace\s+ps\b", code_text)),
                "has_doxygen_file_brief": "@file" in text and "@brief" in text,
            }
        )
        for line_number, line in enumerate(code_text.splitlines(), start=1):
            match = INCLUDE_RE.match(line)
            if match:
                include = match.group(1).replace("\\", "/")
                for forbidden in FORBIDDEN_HOST_INCLUDES:
                    if include == forbidden.rstrip("/") or include.startswith(forbidden):
                        include_violations.append(
                            {
                                "file": header,
                                "line": line_number,
                                "include": include,
                                "reason": f"forbidden host include `{forbidden}`",
                            }
                        )
        for line_number, line in enumerate(text.splitlines(), start=1):
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
        "headers": rows,
        "include_violations": include_violations,
        "implementation_type_occurrences": implementation_type_occurrences,
        "required_types": [
            {
                "name": type_name,
                "declared_in_namespace_ps": type_declared_in_namespace(
                    combined_code, type_name, "ps"
                ),
            }
            for type_name in REQUIRED_PUBLIC_TYPES
        ],
        "required_functions": [
            {
                "name": function_name,
                "declared_in_namespace_ps": symbol_declared_in_namespace(
                    combined_code, function_name, "ps"
                ),
            }
            for function_name in REQUIRED_PUBLIC_FUNCTIONS
        ],
    }


def inspect_implementation(repo: Path) -> dict[str, Any]:
    implementation = repo / HOST_IMPLEMENTATION
    test = repo / HOST_TEST
    cmake_text = (repo / "CMakeLists.txt").read_text(encoding="utf-8")
    impl_text = implementation.read_text(encoding="utf-8") if implementation.exists() else ""
    return {
        "implementation_exists": implementation.exists(),
        "implementation_in_cmake": HOST_IMPLEMENTATION in cmake_text,
        "test_exists": test.exists(),
        "test_in_cmake": HOST_TEST in cmake_text and "test_host_adapter" in cmake_text,
        "implementation_includes_host_header": (
            '#include "photospider/host/host.hpp"' in impl_text
        ),
        "implementation_uses_interaction_service": "InteractionService" in impl_text,
        "implementation_uses_kernel": "Kernel" in impl_text,
    }


def compile_probe_include_flags(repo: Path) -> list[str]:
    flags = [f"-I{repo / 'include'}", f"-I{repo / 'src'}"]
    pkg_config = shutil.which("pkg-config")
    if pkg_config is not None:
        for package in ("yaml-cpp", "opencv4"):
            completed = subprocess.run(
                [pkg_config, "--cflags", package],
                cwd=repo,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )
            if completed.returncode == 0:
                flags.extend(shlex.split(completed.stdout))
    for include_dir in (
        Path("/opt/homebrew/include"),
        Path("/usr/local/include"),
        Path("/usr/include/opencv4"),
        Path("/opt/homebrew/opt/opencv/include/opencv4"),
        Path("/usr/local/opt/opencv/include/opencv4"),
    ):
        if include_dir.exists():
            flag = f"-I{include_dir}"
            if flag not in flags:
                flags.append(flag)
    return flags


def run_public_internal_include_probe(repo: Path, out: Path) -> dict[str, Any]:
    source = out / "public_internal_include_probe.cpp"
    source.write_text(
        "\n".join(
            [
                '#include "photospider/host/host.hpp"',
                '#include "kernel/scheduler/scheduler_task_runtime.hpp"',
                "",
                "int main() { return 0; }",
                "",
            ]
        ),
        encoding="utf-8",
    )
    compiler = shutil.which("c++") or shutil.which("clang++") or shutil.which("g++")
    if compiler is None:
        return {
            "source": rel(repo, source),
            "command": [],
            "returncode": None,
            "stdout": "",
            "stderr": "no C++ compiler found on PATH",
            "passed": False,
        }
    command = [
        compiler,
        "-std=c++17",
        "-fsyntax-only",
        *compile_probe_include_flags(repo),
        str(source),
    ]
    completed = subprocess.run(
        command,
        cwd=repo,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    return {
        "source": rel(repo, source),
        "command": command,
        "returncode": completed.returncode,
        "stdout": completed.stdout,
        "stderr": completed.stderr,
        "passed": completed.returncode == 0,
    }


def write_json(path: Path, payload: dict[str, Any]) -> None:
    path.write_text(
        json.dumps(payload, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )


def make_compare(actual: dict[str, Any], expected: dict[str, Any]) -> tuple[bool, str]:
    checks = {
        "host headers exist": all(
            row["exists"] for row in actual["host_headers"]["headers"]
        )
        == expected["host_headers"]["all_exist"],
        "host headers use namespace ps": all(
            row["uses_namespace_ps"] for row in actual["host_headers"]["headers"]
        )
        == expected["host_headers"]["all_use_namespace_ps"],
        "host headers have file doxygen": all(
            row["has_doxygen_file_brief"]
            for row in actual["host_headers"]["headers"]
        )
        == expected["host_headers"]["all_have_doxygen_file_brief"],
        "host headers avoid forbidden includes": (
            len(actual["host_headers"]["include_violations"])
            == expected["host_headers"]["include_violations"]
        ),
        "host headers avoid implementation-only types": (
            len(actual["host_headers"]["implementation_type_occurrences"])
            == expected["host_headers"]["implementation_type_occurrences"]
        ),
        "required public types declared": all(
            row["declared_in_namespace_ps"]
            for row in actual["host_headers"]["required_types"]
        )
        == expected["host_headers"]["all_required_types_declared"],
        "required public functions declared": all(
            row["declared_in_namespace_ps"]
            for row in actual["host_headers"]["required_functions"]
        )
        == expected["host_headers"]["all_required_functions_declared"],
        "embedded implementation wired": all(actual["implementation"].values())
        == expected["implementation"]["all_wired"],
        "public host header compiles with internal scheduler runtime": actual[
            "public_internal_include_probe"
        ]["passed"]
        == expected["public_internal_include_probe"]["passed"],
    }
    passed = all(checks.values())
    lines = ["phase2_host_adapter_scan"]
    lines.extend(
        f"{'PASS' if ok else 'FAIL'} {name}" for name, ok in checks.items()
    )
    lines.append(f"overall={'PASS' if passed else 'FAIL'}")
    return passed, "\n".join(lines) + "\n"


def make_summary(out: Path, actual: dict[str, Any], passed: bool) -> str:
    host_headers = actual["host_headers"]
    implementation = actual["implementation"]
    return "\n".join(
        [
            "# codebase-refactor phase-2 host adapter evidence",
            "",
            "## Test objective",
            "",
            "Verify issue #29 introduced a public `ps::Host` seam under",
            "`include/photospider/host` plus an embedded adapter implementation",
            "without exposing Kernel, GraphModel, GraphRuntime, ComputeService,",
            "InteractionService, OpenCV, or yaml-cpp through Host public headers.",
            "",
            "## Evidence files",
            "",
            f"- `expected.json`: `{out / 'expected.json'}`",
            f"- `actual.json`: `{out / 'actual.json'}`",
            f"- `compare.log`: `{out / 'compare.log'}`",
            "",
            "## Result",
            "",
            f"- Host headers scanned: {len(host_headers['headers'])}",
            f"- Host include violations: {len(host_headers['include_violations'])}",
            "- Implementation-only type occurrences: "
            f"{len(host_headers['implementation_type_occurrences'])}",
            "- Embedded implementation wired in CMake: "
            f"{implementation['implementation_in_cmake']}",
            f"- Focused Host test wired in CMake: {implementation['test_in_cmake']}",
            "- Public/internal include probe: "
            f"{actual['public_internal_include_probe']['passed']}",
            f"- Overall: {'PASS' if passed else 'FAIL'}",
            "",
            "## Interpretation",
            "",
            "The scan proves the phase-2 host adapter seam is present, declares",
            "`ps::Host` and `create_embedded_host` in namespace `ps`, keeps the",
            "public Host header set self-contained through public value headers,",
            "and confines Kernel/InteractionService usage to the embedded",
            "implementation file. The include probe compiles the public Host",
            "header next to the internal scheduler runtime header, proving the",
            "public Host snapshot names do not collide with backend scheduler",
            "types. It complements the focused C++ test by checking the source",
            "boundary directly.",
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
        "host_headers": inspect_host_headers(repo),
        "implementation": inspect_implementation(repo),
        "public_internal_include_probe": run_public_internal_include_probe(
            repo, out
        ),
    }
    expected = {
        "host_headers": {
            "all_exist": True,
            "all_use_namespace_ps": True,
            "all_have_doxygen_file_brief": True,
            "include_violations": 0,
            "implementation_type_occurrences": 0,
            "all_required_types_declared": True,
            "all_required_functions_declared": True,
        },
        "implementation": {"all_wired": True},
        "public_internal_include_probe": {"passed": True},
    }
    passed, compare = make_compare(actual, expected)

    write_json(out / "expected.json", expected)
    write_json(out / "actual.json", actual)
    (out / "compare.log").write_text(compare, encoding="utf-8")
    (out / "summary.md").write_text(
        make_summary(out, actual, passed) + "\n", encoding="utf-8"
    )
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
