#!/usr/bin/env python3
"""Discover and run CTest build smokes through the JSON object model.

The planner consumes ``ctest --show-only=json-v1`` rather than human-readable
CTest output.  It validates the complete test inventory, selects the exact
``build-smoke`` label, and emits a deterministic GitHub Actions matrix.  The
runner repeats discovery immediately before execution and selects one test by
its numeric CTest index, avoiding shell interpolation and test-name regular
expressions.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import re
import shlex
import subprocess
import sys
from typing import Any, Iterable, Mapping, Sequence


BUILD_SMOKE_LABEL = "build-smoke"
MAX_MATRIX_BYTES = 64 * 1024
MAX_TEST_NAME_BYTES = 4096
MAX_BUILD_SMOKES = 256


def _test_sort_key(test: "TestRecord") -> tuple[str, str]:
    """Return a case-insensitive stable key with exact-name tie breaking."""

    return test.name.casefold(), test.name


class InventoryError(ValueError):
    """Report malformed or unsafe CTest build-smoke inventory state."""


@dataclass(frozen=True)
class TestRecord:
    """Represent one validated test in CTest's stable inventory order."""

    index: int
    name: str
    labels: tuple[str, ...]
    disabled: bool
    has_command: bool


@dataclass(frozen=True)
class Inventory:
    """Hold the validated JSON payload and its ordered test records."""

    payload: Mapping[str, Any]
    tests: tuple[TestRecord, ...]

    def build_smokes(self, label: str = BUILD_SMOKE_LABEL) -> tuple[TestRecord, ...]:
        """Return build smokes sorted deterministically by exact test name."""

        selected = tuple(
            sorted(
                (test for test in self.tests if label in test.labels),
                key=_test_sort_key,
            )
        )
        if not selected:
            raise InventoryError(
                f"CTest inventory has no tests with exact label {label!r}."
            )
        if len(selected) > MAX_BUILD_SMOKES:
            raise InventoryError(
                f"CTest inventory has {len(selected)} build smokes; "
                f"the supported maximum is {MAX_BUILD_SMOKES}."
            )
        for test in selected:
            if test.disabled:
                raise InventoryError(
                    f"Build smoke {test.name!r} is disabled and would not execute."
                )
            if not test.has_command:
                raise InventoryError(
                    f"Build smoke {test.name!r} has no executable CTest command."
                )
        return selected

    def require_selected(
        self, test_name: str, label: str = BUILD_SMOKE_LABEL
    ) -> TestRecord:
        """Require one exact test name to remain uniquely labelled."""

        matches = tuple(test for test in self.tests if test.name == test_name)
        if not matches:
            raise InventoryError(
                f"Selected build smoke {test_name!r} is absent from CTest inventory."
            )
        if len(matches) != 1:
            raise InventoryError(
                f"Selected build smoke {test_name!r} is not unique."
            )
        selected = matches[0]
        if label not in selected.labels:
            raise InventoryError(
                f"Selected test {test_name!r} lacks exact label {label!r}."
            )
        if selected.disabled:
            raise InventoryError(
                f"Selected build smoke {test_name!r} is disabled."
            )
        if not selected.has_command:
            raise InventoryError(
                f"Selected build smoke {test_name!r} has no CTest command."
            )
        return selected


def _require_mapping(value: Any, context: str) -> Mapping[str, Any]:
    """Return ``value`` as a mapping or raise a contextual inventory error."""

    if not isinstance(value, dict):
        raise InventoryError(f"{context} must be a JSON object.")
    return value


def _parse_properties(
    test_name: str, raw_properties: Any
) -> Mapping[str, Any]:
    """Validate one CTest property's array and reject duplicate property names."""

    if raw_properties is None:
        return {}
    if not isinstance(raw_properties, list):
        raise InventoryError(
            f"CTest properties for {test_name!r} must be an array."
        )
    properties: dict[str, Any] = {}
    for index, raw_property in enumerate(raw_properties):
        property_object = _require_mapping(
            raw_property, f"CTest property {index} for {test_name!r}"
        )
        property_name = property_object.get("name")
        if not isinstance(property_name, str) or not property_name:
            raise InventoryError(
                f"CTest property {index} for {test_name!r} has no valid name."
            )
        if "value" not in property_object:
            raise InventoryError(
                f"CTest property {property_name!r} for {test_name!r} "
                "has no value."
            )
        if property_name in properties:
            raise InventoryError(
                f"CTest test {test_name!r} repeats property {property_name!r}."
            )
        properties[property_name] = property_object["value"]
    return properties


def _parse_labels(test_name: str, properties: Mapping[str, Any]) -> tuple[str, ...]:
    """Validate CTest's LABELS property as a unique array of nonempty strings."""

    if "LABELS" not in properties:
        return ()
    raw_labels = properties["LABELS"]
    if not isinstance(raw_labels, list):
        raise InventoryError(
            f"CTest LABELS for {test_name!r} must be an array."
        )
    labels: list[str] = []
    seen: set[str] = set()
    for raw_label in raw_labels:
        if not isinstance(raw_label, str) or not raw_label:
            raise InventoryError(
                f"CTest LABELS for {test_name!r} contains an invalid label."
            )
        if raw_label in seen:
            raise InventoryError(
                f"CTest LABELS for {test_name!r} repeats {raw_label!r}."
            )
        seen.add(raw_label)
        labels.append(raw_label)
    return tuple(labels)


def parse_inventory(raw_json: bytes | str) -> Inventory:
    """Parse and validate CTest's complete ``json-v1`` inventory."""

    if isinstance(raw_json, bytes):
        try:
            inventory_text = raw_json.decode("utf-8")
        except UnicodeDecodeError as error:
            raise InventoryError(
                "CTest inventory is not valid UTF-8."
            ) from error
    elif isinstance(raw_json, str):
        inventory_text = raw_json
    else:
        raise InventoryError("CTest inventory must be UTF-8 text.")

    try:
        raw_payload = json.loads(inventory_text)
    except json.JSONDecodeError as error:
        raise InventoryError(
            f"CTest inventory is not valid JSON: {error.msg}."
        ) from error
    payload = _require_mapping(raw_payload, "CTest inventory root")
    if payload.get("kind") != "ctestInfo":
        raise InventoryError("CTest inventory kind must be 'ctestInfo'.")
    version = _require_mapping(payload.get("version"), "CTest inventory version")
    major = version.get("major")
    minor = version.get("minor")
    if (
        not isinstance(major, int)
        or isinstance(major, bool)
        or major != 1
        or not isinstance(minor, int)
        or isinstance(minor, bool)
        or minor < 0
    ):
        raise InventoryError(
            "CTest inventory must use the supported json-v1 object model."
        )

    raw_tests = payload.get("tests")
    if not isinstance(raw_tests, list) or not raw_tests:
        raise InventoryError("CTest inventory must contain at least one test.")

    records: list[TestRecord] = []
    seen_names: set[str] = set()
    for index, raw_test in enumerate(raw_tests, start=1):
        test_object = _require_mapping(raw_test, f"CTest test {index}")
        test_name = test_object.get("name")
        if not isinstance(test_name, str) or not test_name:
            raise InventoryError(f"CTest test {index} has no valid name.")
        if "\0" in test_name:
            raise InventoryError(f"CTest test {index} contains a NUL in its name.")
        if len(test_name.encode("utf-8")) > MAX_TEST_NAME_BYTES:
            raise InventoryError(
                f"CTest test {test_name!r} exceeds {MAX_TEST_NAME_BYTES} UTF-8 bytes."
            )
        if test_name in seen_names:
            raise InventoryError(f"CTest test name {test_name!r} is duplicated.")
        seen_names.add(test_name)

        properties = _parse_properties(test_name, test_object.get("properties"))
        labels = _parse_labels(test_name, properties)
        disabled_value = properties.get("DISABLED", False)
        if not isinstance(disabled_value, bool):
            raise InventoryError(
                f"CTest DISABLED for {test_name!r} must be boolean."
            )
        command = test_object.get("command")
        has_command = (
            isinstance(command, list)
            and bool(command)
            and bool(command[0])
            and all(
                isinstance(argument, str) and "\0" not in argument
                for argument in command
            )
        )
        records.append(
            TestRecord(
                index=index,
                name=test_name,
                labels=labels,
                disabled=disabled_value,
                has_command=has_command,
            )
        )
    return Inventory(payload=payload, tests=tuple(records))


def _artifact_key(test_name: str) -> str:
    """Return a deterministic GitHub-artifact-safe key for one test name."""

    slug = re.sub(r"[^a-z0-9]+", "-", test_name.lower()).strip("-")
    slug = (slug[:40].rstrip("-") or "test")
    digest = hashlib.sha256(test_name.encode("utf-8")).hexdigest()[:12]
    return f"{slug}-{digest}"


def build_matrix(
    build_smokes: Iterable[TestRecord],
) -> tuple[str, tuple[TestRecord, ...]]:
    """Serialize a deterministic, nonempty GitHub Actions include matrix."""

    ordered = tuple(sorted(build_smokes, key=_test_sort_key))
    if not ordered:
        raise InventoryError("Refusing to emit an empty build-smoke matrix.")
    entries: list[dict[str, str]] = []
    artifact_keys: set[str] = set()
    for test in ordered:
        artifact_key = _artifact_key(test.name)
        if artifact_key in artifact_keys:
            raise InventoryError(
                f"Build-smoke artifact key collision for {test.name!r}."
            )
        artifact_keys.add(artifact_key)
        entries.append({"artifact": artifact_key, "test": test.name})
    matrix = json.dumps(
        {"include": entries},
        ensure_ascii=True,
        separators=(",", ":"),
        sort_keys=True,
    )
    if len(matrix.encode("utf-8")) > MAX_MATRIX_BYTES:
        raise InventoryError(
            f"Build-smoke matrix exceeds {MAX_MATRIX_BYTES} bytes."
        )
    return matrix, ordered


def query_ctest(
    ctest_executable: str, build_dir: Path, config: str | None
) -> Inventory:
    """Run CTest JSON discovery with an argv-only subprocess boundary."""

    command = [
        ctest_executable,
        "--test-dir",
        str(build_dir),
        "--show-only=json-v1",
    ]
    if config:
        command.extend(["-C", config])
    completed = subprocess.run(
        command,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if completed.returncode != 0:
        diagnostic = completed.stderr.decode("utf-8", errors="replace").strip()
        raise InventoryError(
            "CTest JSON discovery failed"
            + (f": {diagnostic}" if diagnostic else ".")
        )
    return parse_inventory(completed.stdout)


def _write_text(path: Path, value: str) -> None:
    """Write one UTF-8 artifact after creating its parent directory."""

    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(value, encoding="utf-8")


def write_inventory(path: Path, inventory: Inventory) -> None:
    """Write normalized CTest JSON for durable CI diagnostics."""

    normalized = json.dumps(
        inventory.payload,
        ensure_ascii=True,
        indent=2,
        sort_keys=True,
    )
    _write_text(path, normalized + "\n")


def write_names(path: Path, build_smokes: Sequence[TestRecord]) -> None:
    """Write exact test names as NUL-delimited UTF-8 records."""

    path.parent.mkdir(parents=True, exist_ok=True)
    encoded = b"".join(test.name.encode("utf-8") + b"\0" for test in build_smokes)
    path.write_bytes(encoded)


def plan(args: argparse.Namespace) -> int:
    """Discover build smokes and write deterministic planner artifacts."""

    inventory = query_ctest(
        args.ctest_executable, args.build_dir, args.config
    )
    build_smokes = inventory.build_smokes(args.label)
    matrix, ordered = build_matrix(build_smokes)
    write_inventory(args.inventory_output, inventory)
    _write_text(args.matrix_output, matrix + "\n")
    if args.names_output is not None:
        write_names(args.names_output, ordered)

    print(
        f"Discovered {len(ordered)} build smokes with exact label "
        f"{args.label!r}:"
    )
    for test in ordered:
        print(json.dumps(test.name, ensure_ascii=True))
    return 0


def run_selected(args: argparse.Namespace) -> int:
    """Revalidate and run one labelled CTest entry by exact numeric index."""

    inventory = query_ctest(
        args.ctest_executable, args.build_dir, args.config
    )
    selected = inventory.require_selected(args.test_name, args.label)
    write_inventory(args.inventory_output, inventory)
    selection = {
        "index": selected.index,
        "label": args.label,
        "test": selected.name,
    }
    _write_text(
        args.selection_output,
        json.dumps(selection, ensure_ascii=True, indent=2, sort_keys=True) + "\n",
    )

    command = [
        args.ctest_executable,
        "--output-on-failure",
        "--test-dir",
        str(args.build_dir),
        "-I",
        f"{selected.index},{selected.index}",
    ]
    if args.config:
        command.extend(["-C", args.config])
    print("$ " + shlex.join(command), flush=True)
    completed = subprocess.run(command, check=False)
    return completed.returncode


def _add_common_arguments(parser: argparse.ArgumentParser) -> None:
    """Add shared CTest discovery arguments to one subcommand parser."""

    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--ctest-executable", default="ctest")
    parser.add_argument("--config")
    parser.add_argument("--label", default=BUILD_SMOKE_LABEL)
    parser.add_argument("--inventory-output", type=Path, required=True)


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    """Parse planner or runner command-line arguments."""

    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    plan_parser = subparsers.add_parser(
        "plan", help="discover labelled tests and emit a CI matrix"
    )
    _add_common_arguments(plan_parser)
    plan_parser.add_argument("--matrix-output", type=Path, required=True)
    plan_parser.add_argument("--names-output", type=Path)
    plan_parser.set_defaults(handler=plan)

    run_parser = subparsers.add_parser(
        "run", help="revalidate and run one exact labelled test"
    )
    _add_common_arguments(run_parser)
    run_parser.add_argument("--test-name", required=True)
    run_parser.add_argument("--selection-output", type=Path, required=True)
    run_parser.set_defaults(handler=run_selected)
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    """Execute the requested planner or runner command."""

    args = parse_args(sys.argv[1:] if argv is None else argv)
    try:
        return args.handler(args)
    except (InventoryError, OSError) as error:
        print(f"Build-smoke inventory error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
