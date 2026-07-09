#!/usr/bin/env python3
import argparse
import json
from pathlib import Path


FORBIDDEN_PUBLIC = [
    "compute_node_no_recurse",
    "compute_internal",
    "compute_high_precision_update",
    "compute_real_time_update",
    "clear_timing_results",
]

ALLOWED_PUBLIC = [
    "ComputeService(",
    "compute(",
    "compute_parallel(",
]


def line_occurrences(path, needle):
    rows = []
    text = path.read_text(encoding="utf-8")
    for index, line in enumerate(text.splitlines(), start=1):
        if needle in line:
            rows.append(
                {"file": str(path), "line": index, "text": line.strip()}
            )
    return rows


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", required=True)
    parser.add_argument("--out", required=True)
    args = parser.parse_args()

    repo = Path(args.repo).resolve()
    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)

    header = repo / "src/kernel/services/compute_service.hpp"
    source = repo / "src/kernel/services/compute_service.cpp"
    header_text = header.read_text(encoding="utf-8")
    public_start = header_text.index(" public:")
    private_start = header_text.index(" private:", public_start)
    public_section = header_text[public_start:private_start]

    public_forbidden = [
        name for name in FORBIDDEN_PUBLIC if name in public_section
    ]
    public_allowed_present = [
        name for name in ALLOWED_PUBLIC if name in public_section
    ]

    compute_node_no_recurse_occurrences = []
    for path in [header, source]:
        compute_node_no_recurse_occurrences.extend(
            line_occurrences(path, "compute_node_no_recurse")
        )

    allowed_internal_files = {
        str(header),
        str(source),
        str(repo / "src/kernel/services/compute-service/compute_task_dispatcher.cpp"),
        str(repo / "src/kernel/services/compute-service/compute_task_dispatcher.hpp"),
    }
    external_helper_occurrences = []
    for root_name in ["include", "src", "tests"]:
        root = repo / root_name
        for path in root.rglob("*"):
            if path.suffix not in {".hpp", ".h", ".cpp", ".cc", ".cxx"}:
                continue
            for name in FORBIDDEN_PUBLIC:
                for row in line_occurrences(path, name):
                    if str(path) not in allowed_internal_files:
                        external_helper_occurrences.append(row)

    actual = {
        "public_forbidden": public_forbidden,
        "public_allowed_present": public_allowed_present,
        "compute_node_no_recurse_occurrences": compute_node_no_recurse_occurrences,
        "external_helper_occurrences": external_helper_occurrences,
    }
    expected = {
        "public_forbidden": [],
        "public_allowed_minimum": ALLOWED_PUBLIC,
        "compute_node_no_recurse_occurrences": [],
        "external_helper_occurrences": [],
    }
    pass_all = (
        public_forbidden == []
        and set(ALLOWED_PUBLIC).issubset(set(public_allowed_present))
        and compute_node_no_recurse_occurrences == []
        and external_helper_occurrences == []
    )

    (out / "expected.json").write_text(
        json.dumps(expected, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    (out / "actual.json").write_text(
        json.dumps(actual, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )

    compare_lines = [
        "ComputeService public API boundary verification",
        "expected public forbidden: []",
        "actual public forbidden: "
        + json.dumps(public_forbidden, ensure_ascii=False),
        "expected compute_node_no_recurse occurrences: []",
        "actual compute_node_no_recurse occurrences: "
        + json.dumps(compute_node_no_recurse_occurrences, ensure_ascii=False),
        "expected external helper occurrences: []",
        "actual external helper occurrences: "
        + json.dumps(external_helper_occurrences, ensure_ascii=False),
        "overall=" + ("PASS" if pass_all else "FAIL"),
    ]
    (out / "compare.log").write_text("\n".join(compare_lines) + "\n",
                                     encoding="utf-8")
    print("overall=" + ("PASS" if pass_all else "FAIL"))
    return 0 if pass_all else 1


if __name__ == "__main__":
    raise SystemExit(main())
