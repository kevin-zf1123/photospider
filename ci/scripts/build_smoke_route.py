#!/usr/bin/env python3
"""Route current-main build smokes to producer, control, or dedicated roles.

CTest remains the authority for concrete test identities. This protected,
explicitly temporary current-main routing lock marks the smoke that must run in
the original producer tree, the installed-package consumer, and the exact
OpenEXR option-off smoke that consumes only producer CMake metadata. Every
remaining discovered entry gets the minimal ``ctest-control`` role. The future
versioned build-profile matrix replaces this lock, and protected cleanup
removes it before closeout.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any


class RoutingError(ValueError):
    """Report malformed or stale current-main build-smoke routing."""


def _unique_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    """Reject duplicate JSON members in protected or generated input."""
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise RoutingError(f"duplicate JSON member: {key}")
        result[key] = value
    return result


def _load(path: Path) -> Any:
    """Load strict UTF-8 JSON with duplicate-member rejection."""
    try:
        return json.loads(
            path.read_text(encoding="utf-8"), object_pairs_hook=_unique_object
        )
    except (OSError, UnicodeError, json.JSONDecodeError, RoutingError) as error:
        raise RoutingError(f"cannot load strict JSON {path}: {error}") from error


def route(
    matrix_path: Path, lock_path: Path
) -> tuple[dict[str, Any], dict[str, Any], dict[str, Any], list[str]]:
    """Partition one validated matrix by the protected current-main roles.

    Args:
        matrix_path: Compact matrix emitted by ``build_smoke_inventory.py``.
        lock_path: Protected temporary producer-role lock.

    Returns:
        Default control matrix, installed-package matrix, OpenEXR-metadata
        matrix, and sorted producer tests. The four results are disjoint and
        exhaust the input matrix.

    Raises:
        RoutingError: Schemas, fields, ordering, identities, or coverage differ.
    """
    matrix = _load(matrix_path)
    lock = _load(lock_path)
    if not isinstance(lock, dict) or set(lock) != {
        "dedicated_consumer_roles",
        "default_consumer_role",
        "producer_tests",
        "schema",
    }:
        raise RoutingError("build-smoke routing lock fields differ")
    if lock["schema"] != "photospider-build-smoke-routing-v3":
        raise RoutingError("unknown build-smoke routing schema")
    if lock["default_consumer_role"] != "ctest-control":
        raise RoutingError("unknown build-smoke consumer artifact role")
    producer_tests = lock["producer_tests"]
    if (
        not isinstance(producer_tests, list)
        or not producer_tests
        or not all(isinstance(item, str) and item for item in producer_tests)
        or producer_tests != sorted(producer_tests)
        or len(producer_tests) != len(set(producer_tests))
    ):
        raise RoutingError("producer_tests must be a nonempty sorted unique array")
    dedicated_roles = lock["dedicated_consumer_roles"]
    if (
        not isinstance(dedicated_roles, dict)
        or not dedicated_roles
        or list(dedicated_roles) != sorted(dedicated_roles)
        or not all(
            isinstance(test, str)
            and test
            and isinstance(role, str)
            and role in {"installed-package", "openexr-metadata"}
            for test, role in dedicated_roles.items()
        )
    ):
        raise RoutingError(
            "dedicated_consumer_roles must be a sorted nonempty supported-role map"
        )
    if set(producer_tests) & set(dedicated_roles):
        raise RoutingError("producer and dedicated build-smoke routes overlap")
    if not isinstance(matrix, dict) or set(matrix) != {"include"}:
        raise RoutingError("build-smoke matrix fields differ")
    entries = matrix["include"]
    if not isinstance(entries, list) or not entries:
        raise RoutingError("build-smoke matrix is empty")
    seen: set[str] = set()
    consumers: list[dict[str, str]] = []
    installed_package: list[dict[str, str]] = []
    openexr_metadata: list[dict[str, str]] = []
    selected_producers: list[str] = []
    for entry in entries:
        if not isinstance(entry, dict) or set(entry) != {"artifact", "test"}:
            raise RoutingError("build-smoke matrix entry fields differ")
        artifact = entry["artifact"]
        test = entry["test"]
        if (
            not isinstance(artifact, str)
            or not artifact
            or not isinstance(test, str)
            or not test
        ):
            raise RoutingError("build-smoke matrix entry is empty")
        if test in seen:
            raise RoutingError(f"duplicate build-smoke identity: {test}")
        seen.add(test)
        if test in producer_tests:
            selected_producers.append(test)
        elif test in dedicated_roles:
            destination = (
                installed_package
                if dedicated_roles[test] == "installed-package"
                else openexr_metadata
            )
            destination.append(
                {
                    "artifact": artifact,
                    "artifact_role": dedicated_roles[test],
                    "test": test,
                }
            )
        else:
            consumers.append(
                {
                    "artifact": artifact,
                    "artifact_role": lock["default_consumer_role"],
                    "test": test,
                }
            )
    if selected_producers != producer_tests:
        raise RoutingError(
            "protected producer tests differ from discovered CTest inventory"
        )
    if not consumers:
        raise RoutingError("routing removed every build-smoke consumer")
    expected_installed = [
        test for test, role in dedicated_roles.items() if role == "installed-package"
    ]
    expected_openexr = [
        test for test, role in dedicated_roles.items() if role == "openexr-metadata"
    ]
    if [entry["test"] for entry in installed_package] != expected_installed:
        raise RoutingError(
            "protected installed-package tests differ from discovered CTest inventory"
        )
    if [entry["test"] for entry in openexr_metadata] != expected_openexr:
        raise RoutingError(
            "protected OpenEXR metadata tests differ from discovered CTest inventory"
        )
    routed = (
        {entry["test"] for entry in consumers}
        | {entry["test"] for entry in installed_package}
        | {entry["test"] for entry in openexr_metadata}
        | set(selected_producers)
    )
    if routed != seen or sum(
        (
            len(consumers),
            len(installed_package),
            len(openexr_metadata),
            len(selected_producers),
        )
    ) != len(seen):
        raise RoutingError("build-smoke routing is not disjoint and exhaustive")
    return (
        {"include": consumers},
        {"include": installed_package},
        {"include": openexr_metadata},
        selected_producers,
    )


def main() -> int:
    """Parse paths, route the matrix, and write canonical artifacts."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--matrix", type=Path, required=True)
    parser.add_argument("--lock", type=Path, required=True)
    parser.add_argument("--consumer-matrix", type=Path, required=True)
    parser.add_argument("--dedicated-matrix", type=Path, required=True)
    parser.add_argument("--openexr-matrix", type=Path, required=True)
    parser.add_argument("--producer-names", type=Path, required=True)
    arguments = parser.parse_args()
    try:
        consumers, dedicated, openexr, producers = route(
            arguments.matrix, arguments.lock
        )
        arguments.consumer_matrix.parent.mkdir(parents=True, exist_ok=True)
        arguments.consumer_matrix.write_text(
            json.dumps(consumers, sort_keys=True, separators=(",", ":")) + "\n",
            encoding="utf-8",
        )
        arguments.dedicated_matrix.parent.mkdir(parents=True, exist_ok=True)
        arguments.dedicated_matrix.write_text(
            json.dumps(dedicated, sort_keys=True, separators=(",", ":")) + "\n",
            encoding="utf-8",
        )
        arguments.openexr_matrix.parent.mkdir(parents=True, exist_ok=True)
        arguments.openexr_matrix.write_text(
            json.dumps(openexr, sort_keys=True, separators=(",", ":")) + "\n",
            encoding="utf-8",
        )
        arguments.producer_names.parent.mkdir(parents=True, exist_ok=True)
        arguments.producer_names.write_bytes(
            b"".join(name.encode("utf-8") + b"\0" for name in producers)
        )
    except (OSError, RoutingError) as error:
        print(f"build-smoke routing failed: {error}", file=sys.stderr)
        return 1
    print(
        f"Routed {len(consumers['include'])} minimal consumers, "
        f"{len(dedicated['include'])} installed-package consumers, "
        f"{len(openexr['include'])} OpenEXR metadata consumers, and "
        f"{len(producers)} producer-local smokes."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
