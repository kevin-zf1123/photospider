#!/usr/bin/env python3
"""Regress deterministic and fail-closed build-smoke inventory handling."""

from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


MODULE_PATH = Path(__file__).with_name("build_smoke_inventory.py")
SPEC = importlib.util.spec_from_file_location("build_smoke_inventory", MODULE_PATH)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"Cannot load {MODULE_PATH}")
inventory_module = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = inventory_module
SPEC.loader.exec_module(inventory_module)


def test_entry(
    name: str,
    *,
    labels: list[str] | None = None,
    disabled: bool = False,
    command: list[str] | None = None,
) -> dict[str, object]:
    """Create one realistic CTest JSON test entry for a focused regression."""

    properties: list[dict[str, object]] = [
        {"name": "WORKING_DIRECTORY", "value": "/tmp/build"}
    ]
    if labels is not None:
        properties.append({"name": "LABELS", "value": labels})
    if disabled:
        properties.append({"name": "DISABLED", "value": True})
    entry: dict[str, object] = {
        "name": name,
        "properties": properties,
    }
    if command is not None:
        entry["command"] = command
    return entry


def inventory_payload(tests: list[dict[str, object]]) -> bytes:
    """Serialize one minimal CTest json-v1 inventory as UTF-8 bytes."""

    return json.dumps(
        {
            "kind": "ctestInfo",
            "tests": tests,
            "version": {"major": 1, "minor": 0},
        }
    ).encode("utf-8")


class InventoryParsingTest(unittest.TestCase):
    """Exercise strict JSON schema and label validation."""

    def test_matrix_is_sorted_safe_and_round_trippable(self) -> None:
        raw = inventory_payload(
            [
                test_entry(
                    "zeta [consumer]",
                    labels=["build-smoke", "package"],
                    command=["python3", "zeta.py"],
                ),
                test_entry("ordinary", command=["ordinary"]),
                test_entry(
                    'alpha "quoted"\nline',
                    labels=["build-smoke"],
                    command=["python3", "alpha.py"],
                ),
                test_entry(
                    "Unicode-测试",
                    labels=["build-smoke"],
                    command=["python3", "unicode.py"],
                ),
            ]
        )
        inventory = inventory_module.parse_inventory(raw)
        matrix_text, selected = inventory_module.build_matrix(
            inventory.build_smokes()
        )
        matrix = json.loads(matrix_text)

        self.assertEqual(
            [test.name for test in selected],
            ['alpha "quoted"\nline', "Unicode-测试", "zeta [consumer]"],
        )
        self.assertNotIn("\n", matrix_text)
        self.assertEqual(
            [entry["test"] for entry in matrix["include"]],
            [test.name for test in selected],
        )
        artifacts = [entry["artifact"] for entry in matrix["include"]]
        self.assertEqual(len(artifacts), len(set(artifacts)))
        for artifact in artifacts:
            self.assertRegex(artifact, r"^[a-z0-9-]+$")

    def test_duplicate_test_name_fails_closed(self) -> None:
        raw = inventory_payload(
            [
                test_entry("duplicate", command=["one"]),
                test_entry("duplicate", command=["two"]),
            ]
        )
        with self.assertRaisesRegex(
            inventory_module.InventoryError, "duplicated"
        ):
            inventory_module.parse_inventory(raw)

    def test_duplicate_labels_property_fails_closed(self) -> None:
        entry = test_entry(
            "smoke", labels=["build-smoke"], command=["smoke"]
        )
        entry["properties"].append(
            {"name": "LABELS", "value": ["build-smoke"]}
        )
        with self.assertRaisesRegex(
            inventory_module.InventoryError, "repeats property"
        ):
            inventory_module.parse_inventory(inventory_payload([entry]))

    def test_malformed_label_value_fails_closed(self) -> None:
        entry = test_entry("smoke", command=["smoke"])
        entry["properties"].append(
            {"name": "LABELS", "value": "build-smoke"}
        )
        with self.assertRaisesRegex(
            inventory_module.InventoryError, "must be an array"
        ):
            inventory_module.parse_inventory(inventory_payload([entry]))

    def test_empty_build_smoke_set_fails_closed(self) -> None:
        inventory = inventory_module.parse_inventory(
            inventory_payload([test_entry("ordinary", command=["ordinary"])])
        )
        with self.assertRaisesRegex(
            inventory_module.InventoryError, "no tests with exact label"
        ):
            inventory.build_smokes()

    def test_disabled_or_commandless_build_smoke_fails_closed(self) -> None:
        disabled = inventory_module.parse_inventory(
            inventory_payload(
                [
                    test_entry(
                        "disabled",
                        labels=["build-smoke"],
                        disabled=True,
                        command=["disabled"],
                    )
                ]
            )
        )
        with self.assertRaisesRegex(
            inventory_module.InventoryError, "disabled"
        ):
            disabled.build_smokes()

        commandless = inventory_module.parse_inventory(
            inventory_payload(
                [test_entry("commandless", labels=["build-smoke"])]
            )
        )
        with self.assertRaisesRegex(
            inventory_module.InventoryError, "no executable CTest command"
        ):
            commandless.build_smokes()

    def test_unsupported_schema_and_invalid_json_fail_closed(self) -> None:
        with self.assertRaisesRegex(
            inventory_module.InventoryError, "not valid JSON"
        ):
            inventory_module.parse_inventory(b"{")
        wrong_kind = json.dumps(
            {
                "kind": "cmakeFiles",
                "tests": [test_entry("smoke", command=["smoke"])],
                "version": {"major": 1, "minor": 0},
            }
        )
        with self.assertRaisesRegex(
            inventory_module.InventoryError, "kind"
        ):
            inventory_module.parse_inventory(wrong_kind)
        boolean_major = json.dumps(
            {
                "kind": "ctestInfo",
                "tests": [test_entry("smoke", command=["smoke"])],
                "version": {"major": True, "minor": 0},
            }
        )
        with self.assertRaisesRegex(
            inventory_module.InventoryError, "json-v1"
        ):
            inventory_module.parse_inventory(boolean_major)


class ExactExecutionTest(unittest.TestCase):
    """Prove execution revalidates labels and never interpolates test names."""

    def test_selected_test_uses_only_validated_numeric_index(self) -> None:
        adversarial_name = "smoke; $(touch should-not-run) [.*]\nnext"
        payload = inventory_payload(
            [
                test_entry("ordinary", command=["ordinary"]),
                test_entry(
                    adversarial_name,
                    labels=["build-smoke"],
                    command=["python3", "runner.py"],
                ),
            ]
        )
        query_result = subprocess.CompletedProcess(
            args=["ctest"], returncode=0, stdout=payload, stderr=b""
        )
        run_result = subprocess.CompletedProcess(
            args=["ctest"], returncode=0
        )
        with tempfile.TemporaryDirectory() as temporary_directory:
            temporary_root = Path(temporary_directory)
            args = inventory_module.parse_args(
                [
                    "run",
                    "--build-dir",
                    str(temporary_root / "build"),
                    "--inventory-output",
                    str(temporary_root / "inventory.json"),
                    "--selection-output",
                    str(temporary_root / "selection.json"),
                    "--test-name",
                    adversarial_name,
                ]
            )
            with mock.patch.object(
                inventory_module.subprocess,
                "run",
                side_effect=[query_result, run_result],
            ) as run_mock:
                self.assertEqual(inventory_module.run_selected(args), 0)

            execution_argv = run_mock.call_args_list[1].args[0]
            self.assertIn("-I", execution_argv)
            self.assertIn("2,2", execution_argv)
            self.assertNotIn("-L", execution_argv)
            self.assertNotIn(adversarial_name, execution_argv)
            selection = json.loads(
                (temporary_root / "selection.json").read_text(encoding="utf-8")
            )
            self.assertEqual(selection["test"], adversarial_name)

    def test_selected_unlabelled_test_fails_before_execution(self) -> None:
        payload = inventory_payload(
            [test_entry("ordinary", command=["ordinary"])]
        )
        query_result = subprocess.CompletedProcess(
            args=["ctest"], returncode=0, stdout=payload, stderr=b""
        )
        with tempfile.TemporaryDirectory() as temporary_directory:
            temporary_root = Path(temporary_directory)
            args = inventory_module.parse_args(
                [
                    "run",
                    "--build-dir",
                    str(temporary_root / "build"),
                    "--inventory-output",
                    str(temporary_root / "inventory.json"),
                    "--selection-output",
                    str(temporary_root / "selection.json"),
                    "--test-name",
                    "ordinary",
                ]
            )
            with mock.patch.object(
                inventory_module.subprocess,
                "run",
                return_value=query_result,
            ) as run_mock:
                with self.assertRaisesRegex(
                    inventory_module.InventoryError, "lacks exact label"
                ):
                    inventory_module.run_selected(args)
            run_mock.assert_called_once()


if __name__ == "__main__":
    unittest.main(verbosity=2)
