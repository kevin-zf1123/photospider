#!/usr/bin/env python3
"""Verify fail-closed invariants in the maintained trust-bundle generator."""

from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from generate_plugin_trust_bundle import write_plugin_trust_bundle


class GeneratePluginTrustBundleTest(unittest.TestCase):
    """Exercises generator validation that must precede signing side effects."""

    def test_rejects_same_content_role_across_package_generations(self) -> None:
        """Reject one operation digest assigned to two signed identities."""

        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            first_artifact = root / "first-plugin"
            second_artifact = root / "second-plugin"
            first_artifact.write_bytes(b"identical native artifact")
            second_artifact.write_bytes(b"identical native artifact")
            manifest = root / "manifest.txt"

            with self.assertRaisesRegex(
                ValueError, "duplicate test trust manifest content-role entry"
            ):
                write_plugin_trust_bundle(
                    openssl="signing-must-not-run",
                    private_key=root / "unused-private-key.pem",
                    manifest=manifest,
                    signature=root / "signature.hex",
                    bad_signature=None,
                    trust_root="generator-content-role-test",
                    entries=[
                        [
                            "operation",
                            "11111111111111111111111111111111",
                            "1",
                            str(first_artifact),
                        ],
                        [
                            "operation",
                            "22222222222222222222222222222222",
                            "2",
                            str(second_artifact),
                        ],
                    ],
                )

            self.assertFalse(manifest.exists())


if __name__ == "__main__":
    unittest.main()
