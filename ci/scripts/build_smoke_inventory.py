#!/usr/bin/env python3
"""@file build_smoke_inventory.py
@brief Discover and run CTest build smokes through the JSON object model.

The planner consumes ``ctest --show-only=json-v1`` rather than human-readable
CTest output.  It validates the complete test inventory, selects the exact
``build-smoke`` label, and emits a deterministic GitHub Actions matrix.  The
runner repeats discovery immediately before execution and selects one test by
its numeric CTest index, avoiding shell interpolation and test-name regular
expressions.

@note All discovery and artifact I/O is synchronous and process-local. The
  module keeps no cache or shared mutable state between invocations.
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


#: @brief Exact CTest label that assigns a test to the build-smoke shard.
#: @note This process-lifetime constant must match ci/scripts/common.sh.
BUILD_SMOKE_LABEL = "build-smoke"
#: @brief Maximum UTF-8 size accepted for one serialized workflow matrix.
#: @note The bound limits data passed through GitHub Actions job outputs.
MAX_MATRIX_BYTES = 64 * 1024
#: @brief Maximum UTF-8 size accepted for one exact CTest test name.
#: @note The bound is checked before names reach artifacts or workflow outputs.
MAX_TEST_NAME_BYTES = 4096
#: @brief Maximum number of independently scheduled build-smoke matrix entries.
#: @note The bound applies to strict and allow-empty planning alike.
MAX_BUILD_SMOKES = 256


def _test_sort_key(test: "TestRecord") -> tuple[str, str]:
    """@brief Build the deterministic ordering key for one validated test.

    @param test Validated CTest record whose exact name supplies both key parts.
    @return Case-folded name followed by the exact name for stable tie breaking.
    @throws AttributeError If a caller violates the TestRecord input boundary.
    @note This helper is pure, performs no I/O, and retains no references.
    """

    return test.name.casefold(), test.name


class InventoryError(ValueError):
    """@brief Report malformed or unsafe CTest build-smoke inventory state.

    @note Instances carry only a diagnostic string. They own no external
      resources, caches, subprocesses, or cross-thread state.
    """


@dataclass(frozen=True)
class TestRecord:
    """@brief Represent one validated test in CTest's inventory order.

    @param index One-based numeric position used by CTest's ``-I`` selector.
    @param name Exact validated CTest name.
    @param labels Ordered, unique labels parsed from the LABELS property.
    @param disabled Whether CTest marks the test as disabled.
    @param has_command Whether the JSON entry contains a valid command vector.
    @note Records are immutable value objects. They borrow no JSON containers
      and own no I/O, cache, subprocess, or thread lifetime.
    """

    #: @brief One-based index in the complete CTest JSON test array.
    index: int
    #: @brief Exact test name preserved without shell or regex interpretation.
    name: str
    #: @brief Validated unique CTest labels in source order.
    labels: tuple[str, ...]
    #: @brief True only when CTest's DISABLED property is true.
    disabled: bool
    #: @brief True only when the JSON command is a usable string argv.
    has_command: bool


@dataclass(frozen=True)
class Inventory:
    """@brief Hold one validated CTest JSON payload and its ordered records.

    @param payload Complete validated root object retained for diagnostics.
    @param tests Immutable records in CTest's original one-based index order.
    @note The payload is retained only for the lifetime of this in-memory
      instance. Inventory performs no implicit file I/O or subprocess work.
    """

    #: @brief Complete validated CTest root object used for normalized output.
    payload: Mapping[str, Any]
    #: @brief Immutable validated records in exact CTest inventory order.
    tests: tuple[TestRecord, ...]

    def build_smokes(
        self,
        label: str = BUILD_SMOKE_LABEL,
        *,
        allow_empty: bool = False,
    ) -> tuple[TestRecord, ...]:
        """@brief Select executable build smokes in deterministic name order.

        The method selects the exact label, checks the matrix-size bound, and
        rejects disabled or commandless selected entries. Configuration-time
        preflight may explicitly allow an empty result because POST_BUILD
        GoogleTest discovery has not run yet.

        @param label Exact case-sensitive CTest label to select.
        @param allow_empty Whether an empty selection is valid for non-authoritative
          configuration-time preflight.
        @return Immutable selected records sorted case-insensitively with exact
          name tie breaking.
        @throws InventoryError If strict selection is empty, the selection is too
          large, or a selected test cannot execute.
        @note The method is pure and does not mutate the retained payload.
        """

        selected = tuple(
            sorted(
                (test for test in self.tests if label in test.labels),
                key=_test_sort_key,
            )
        )
        if not selected and not allow_empty:
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
        """@brief Revalidate one exact matrix selection before execution.

        The method resolves the exact name in the complete inventory, then
        requires uniqueness, the exact label, enabled state, and a command.

        @param test_name Exact workflow-selected CTest name.
        @param label Exact case-sensitive label the selection must retain.
        @return The unique executable TestRecord to run by numeric index.
        @throws InventoryError If the name is absent, duplicated, unlabelled,
          disabled, or commandless.
        @note Validation is in-memory and launches no subprocess. The caller may
          execute only after this method returns successfully.
        """

        matches = tuple(test for test in self.tests if test.name == test_name)
        if not matches:
            raise InventoryError(
                f"Selected build smoke {test_name!r} is absent from CTest inventory."
            )
        if len(matches) != 1:
            raise InventoryError(f"Selected build smoke {test_name!r} is not unique.")
        selected = matches[0]
        if label not in selected.labels:
            raise InventoryError(
                f"Selected test {test_name!r} lacks exact label {label!r}."
            )
        if selected.disabled:
            raise InventoryError(f"Selected build smoke {test_name!r} is disabled.")
        if not selected.has_command:
            raise InventoryError(
                f"Selected build smoke {test_name!r} has no CTest command."
            )
        return selected


def _require_mapping(value: Any, context: str) -> Mapping[str, Any]:
    """@brief Require one decoded JSON value to be an object.

    @param value Decoded value to validate.
    @param context Stable diagnostic context for a type mismatch.
    @return The same dictionary through the Mapping interface.
    @throws InventoryError If value is not a JSON object.
    @note The function does not copy or mutate the mapping and performs no I/O.
    """

    if not isinstance(value, dict):
        raise InventoryError(f"{context} must be a JSON object.")
    return value


def _parse_properties(test_name: str, raw_properties: Any) -> Mapping[str, Any]:
    """@brief Parse one CTest property's array into a unique-name mapping.

    @param test_name Exact test name used in contextual diagnostics.
    @param raw_properties Decoded properties array, or None when absent.
    @return Newly owned mapping from property name to decoded value.
    @throws InventoryError If the array, an entry, a name, or a value is
      malformed, or if a property name repeats.
    @note Property values remain decoded JSON objects and are not interpreted
      here. The helper performs no I/O and shares no mutable state.
    """

    if raw_properties is None:
        return {}
    if not isinstance(raw_properties, list):
        raise InventoryError(f"CTest properties for {test_name!r} must be an array.")
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
                f"CTest property {property_name!r} for {test_name!r} " "has no value."
            )
        if property_name in properties:
            raise InventoryError(
                f"CTest test {test_name!r} repeats property {property_name!r}."
            )
        properties[property_name] = property_object["value"]
    return properties


def _parse_labels(test_name: str, properties: Mapping[str, Any]) -> tuple[str, ...]:
    """@brief Validate and normalize one CTest LABELS property.

    @param test_name Exact test name used in contextual diagnostics.
    @param properties Validated property mapping for that test.
    @return Ordered immutable labels, or an empty tuple when LABELS is absent.
    @throws InventoryError If LABELS is not an array, contains an empty or
      non-string value, or repeats a value.
    @note Labels remain case-sensitive exact strings. The helper performs no
      filesystem, subprocess, cache, or thread work.
    """

    if "LABELS" not in properties:
        return ()
    raw_labels = properties["LABELS"]
    if not isinstance(raw_labels, list):
        raise InventoryError(f"CTest LABELS for {test_name!r} must be an array.")
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
    """@brief Parse and validate CTest's complete ``json-v1`` inventory.

    The parser decodes UTF-8/JSON, validates the ctestInfo schema version, then
    validates every test name, property set, label set, disabled flag, and
    command shape while assigning stable one-based indices.

    @param raw_json UTF-8 bytes or text emitted by CTest JSON discovery.
    @return Immutable Inventory retaining the decoded payload and test records.
    @throws InventoryError If encoding, JSON, schema, names, properties, labels,
      disabled state, or command representation is unsafe or malformed.
    @note Parsing is deterministic and in-memory. It performs no external I/O
      and does not retain the caller's byte buffer.
    """

    if isinstance(raw_json, bytes):
        try:
            inventory_text = raw_json.decode("utf-8")
        except UnicodeDecodeError as error:
            raise InventoryError("CTest inventory is not valid UTF-8.") from error
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
            raise InventoryError(f"CTest DISABLED for {test_name!r} must be boolean.")
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
    """@brief Derive a bounded artifact-safe key from one exact test name.

    @param test_name Validated exact CTest name.
    @return Lowercase ASCII slug plus a 12-hex SHA-256 identity suffix.
    @throws UnicodeEncodeError If test_name contains an invalid surrogate.
    @note The digest preserves identity after lossy slug normalization. The
      helper is pure and does not access GitHub or the filesystem.
    """

    slug = re.sub(r"[^a-z0-9]+", "-", test_name.lower()).strip("-")
    slug = slug[:40].rstrip("-") or "test"
    digest = hashlib.sha256(test_name.encode("utf-8")).hexdigest()[:12]
    return f"{slug}-{digest}"


def build_matrix(
    build_smokes: Iterable[TestRecord],
    *,
    allow_empty: bool = False,
) -> tuple[str, tuple[TestRecord, ...]]:
    """@brief Serialize a deterministic GitHub Actions include matrix.

    The function materializes and sorts the input, derives collision-resistant
    artifact keys, emits compact ASCII JSON, and enforces the output-size bound.

    @param build_smokes Validated records selected for independent execution.
    @param allow_empty Whether to emit ``{"include":[]}`` for a
      configuration-time non-authoritative preflight.
    @return Compact JSON matrix and its immutable deterministic record order.
    @throws InventoryError If strict input is empty, artifact keys collide, or
      the serialized matrix exceeds the supported byte bound.
    @note Exact test names remain JSON values and are never evaluated as shell
      or regular-expression syntax. No I/O occurs here.
    """

    ordered = tuple(sorted(build_smokes, key=_test_sort_key))
    if not ordered and not allow_empty:
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
        raise InventoryError(f"Build-smoke matrix exceeds {MAX_MATRIX_BYTES} bytes.")
    return matrix, ordered


def query_ctest(
    ctest_executable: str, build_dir: Path, config: str | None
) -> Inventory:
    """@brief Query and validate one configured CTest tree synchronously.

    @param ctest_executable Executable name or path invoked without a shell.
    @param build_dir Configured build tree passed to ``--test-dir``.
    @param config Optional multi-configuration name passed through ``-C``.
    @return Parsed and validated complete CTest Inventory.
    @throws OSError If the CTest process cannot be started.
    @throws InventoryError If CTest exits nonzero or emits invalid inventory.
    @note Captured stdout/stderr live only for this call. The subprocess is
      awaited before return and no background thread or process survives.
    """

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
            "CTest JSON discovery failed" + (f": {diagnostic}" if diagnostic else ".")
        )
    return parse_inventory(completed.stdout)


def _write_text(path: Path, value: str) -> None:
    """@brief Write one complete UTF-8 artifact with parent creation.

    @param path Destination artifact path.
    @param value Complete text replacing any prior file contents.
    @return None after the synchronous write completes.
    @throws OSError If parent creation or file replacement fails.
    @throws UnicodeError If value cannot be encoded as UTF-8.
    @note The file lifetime is owned by the caller's artifact directory. This
      helper does not cache content or coordinate concurrent writers.
    """

    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(value, encoding="utf-8")


def write_inventory(path: Path, inventory: Inventory) -> None:
    """@brief Persist normalized CTest JSON for CI diagnostics.

    @param path Destination inventory artifact.
    @param inventory Validated inventory whose complete payload is serialized.
    @return None after deterministic UTF-8 output is written.
    @throws OSError If the artifact cannot be created or replaced.
    @throws TypeError If a caller supplies a non-JSON payload despite validation.
    @note Output is diagnostic only and is never read back as execution
      authority in the same process.
    """

    normalized = json.dumps(
        inventory.payload,
        ensure_ascii=True,
        indent=2,
        sort_keys=True,
    )
    _write_text(path, normalized + "\n")


def write_names(path: Path, build_smokes: Sequence[TestRecord]) -> None:
    """@brief Persist exact test names as NUL-delimited UTF-8 records.

    @param path Destination name-inventory artifact.
    @param build_smokes Deterministically ordered validated records.
    @return None after the complete binary inventory is written.
    @throws OSError If parent creation or file replacement fails.
    @throws UnicodeError If a name cannot be encoded as UTF-8.
    @note NUL delimiters preserve embedded newlines and shell metacharacters.
      The caller owns the artifact lifetime and concurrent access discipline.
    """

    path.parent.mkdir(parents=True, exist_ok=True)
    encoded = b"".join(test.name.encode("utf-8") + b"\0" for test in build_smokes)
    path.write_bytes(encoded)


def plan(args: argparse.Namespace) -> int:
    """@brief Discover build smokes and write planner artifacts.

    The handler queries CTest, selects validated labelled entries, serializes
    the matrix, and writes normalized inventory, matrix, and optional NUL names.
    ``args.allow_empty`` is reserved for configuration-time preflight; the
    post-build authority omits it and therefore remains strict.

    @param args Parsed ``plan`` namespace created by parse_args.
    @return Zero after all discovery and artifact writes succeed.
    @throws OSError If CTest or artifact I/O fails.
    @throws InventoryError If inventory or matrix validation fails.
    @note Execution is synchronous and leaves no child process alive. Artifacts
      remain owned by the caller-selected output directory.
    """

    inventory = query_ctest(args.ctest_executable, args.build_dir, args.config)
    build_smokes = inventory.build_smokes(args.label, allow_empty=args.allow_empty)
    matrix, ordered = build_matrix(build_smokes, allow_empty=args.allow_empty)
    write_inventory(args.inventory_output, inventory)
    _write_text(args.matrix_output, matrix + "\n")
    if args.names_output is not None:
        write_names(args.names_output, ordered)

    print(
        f"Discovered {len(ordered)} build smokes with exact label " f"{args.label!r}:"
    )
    for test in ordered:
        print(json.dumps(test.name, ensure_ascii=True))
    return 0


def run_selected(args: argparse.Namespace) -> int:
    """@brief Revalidate and execute one labelled CTest entry by numeric index.

    The handler queries fresh inventory, resolves the exact selected name,
    writes diagnostic inventory and selection artifacts, then invokes CTest
    using only the validated one-based index.

    @param args Parsed ``run`` namespace created by parse_args.
    @return The selected CTest subprocess exit code.
    @throws OSError If discovery, artifact I/O, or execution cannot start.
    @throws InventoryError If fresh inventory or the exact selection is invalid.
    @note A rejected selection returns before the second subprocess call. Test
      names never enter the execution argv or a regular expression.
    """

    inventory = query_ctest(args.ctest_executable, args.build_dir, args.config)
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
    """@brief Register shared discovery arguments on one subcommand parser.

    @param parser Mutable argparse subcommand parser owned by parse_args.
    @return None after registering build, CTest, config, label, and output flags.
    @throws argparse.ArgumentError If a caller pre-registers a conflicting flag.
    @note The parser retains argument definitions for its own lifetime; this
      helper performs no filesystem or subprocess I/O.
    """

    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--ctest-executable", default="ctest")
    parser.add_argument("--config")
    parser.add_argument("--label", default=BUILD_SMOKE_LABEL)
    parser.add_argument("--inventory-output", type=Path, required=True)


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    """@brief Parse one planner or runner command line.

    @param argv Argument sequence excluding the process executable name.
    @return Namespace containing a validated subcommand and handler.
    @throws SystemExit If argparse rejects syntax or a required argument.
    @note Path arguments are converted to Path objects but are not accessed.
      Parser state is local to this call and not cached.
    """

    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    plan_parser = subparsers.add_parser(
        "plan", help="discover labelled tests and emit a CI matrix"
    )
    _add_common_arguments(plan_parser)
    plan_parser.add_argument("--matrix-output", type=Path, required=True)
    plan_parser.add_argument("--names-output", type=Path)
    plan_parser.add_argument(
        "--allow-empty",
        action="store_true",
        help=(
            "allow an empty labelled set only for non-authoritative "
            "configuration-time preflight"
        ),
    )
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
    """@brief Execute the requested inventory subcommand.

    @param argv Optional explicit arguments; None selects ``sys.argv[1:]``.
    @return Handler status, or two after a reported inventory/I/O failure.
    @throws SystemExit If argparse rejects the command line.
    @note Expected operational failures are converted to one stderr diagnostic.
      The function is synchronous and retains no process-global run state.
    """

    args = parse_args(sys.argv[1:] if argv is None else argv)
    try:
        return args.handler(args)
    except (InventoryError, OSError) as error:
        print(f"Build-smoke inventory error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
