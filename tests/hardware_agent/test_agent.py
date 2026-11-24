import copy
from datetime import datetime, timedelta, timezone
import hashlib
from io import BytesIO
import json
from pathlib import Path
import sqlite3
import struct
import subprocess
import sys
import tempfile
from types import SimpleNamespace
from unittest import TestCase, mock

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools" / "hardware_agent"))
import agent
import bench
from github import GitHub, GitHubError, REPO_ID


NOW = datetime(2026, 11, 24, 8, 0, 5, tzinfo=timezone.utc)
SHA = "a" * 40
ID = "0e11ea1f-89d1-4d23-b7f0-0543bc108ac0"
SECOND_ID = "0e11ea1f-89d1-4d23-b7f0-0543bc108ac1"


def manifest(**changes):
    return dict(
        {
            "version": 1, "id": ID, "pr": 21, "commit": SHA, "target": bench.TARGET,
            "operation": "read-only-health", "plan": {"seconds": 1}, "artifact": None,
        },
        **changes,
    )


def request(job=None, comment_id=41, maintainer=agent.MAINTAINER):
    timestamp = NOW.strftime("%Y-%m-%dT%H:%M:%SZ")
    return {
        "id": comment_id,
        "user": {"id": maintainer, "type": "User"},
        "created_at": timestamp,
        "updated_at": timestamp,
        "body": "```roadhog-job\n" + json.dumps(job or manifest()) + "\n```",
    }


class FakeGH:
    def __init__(self, comments):
        self.comments = comments
        self.posts = []
        self.eligible = True
        self.offline = False
        self.archive_matches = True

    def queue(self, since):
        return self.comments

    def eligible_pr(self, number, sha):
        return self.eligible

    def verify_archive(self, artifact, sha):
        return self.archive_matches

    def report(self, number, body, account):
        if self.offline:
            raise GitHubError(503)
        self.posts.append((number, body))


class ManifestTests(TestCase):
    def test_schema_rejects_unknown_or_unsafe_values(self):
        for change in (
            {"operation": "flash"}, {"version": True}, {"pr": False}, {"commit": "main"},
            {"target": "other"}, {"id": "../tmp"}, {"plan": {"seconds": 31}},
            {"plan": {"seconds": True}}, {"plan": {"seconds": 1, "command": "anything"}},
            {"artifact": {"id": 1, "sha256": "a" * 64}}, {"url": "https://example.org"},
            {"operation": "verify-layout", "plan": {"offset": "0x9000"}},
            {"operation": "build-validation", "plan": {}, "artifact": {"id": 4, "sha256": "bad"}},
        ):
            with self.subTest(change=change):
                self.assertIsNone(agent.parse_job(request(manifest(**change)), NOW))

    def test_rejects_untrusted_app_edited_stale_and_duplicate_keys(self):
        approved = request()
        self.assertEqual(agent.parse_job(approved, NOW), manifest())
        duplicate = approved["body"].replace('"version": 1', '"version": 1, "version": 1')
        for override in (
            {"user": {"id": 999, "type": "User"}},
            {"performed_via_github_app": {"id": 123}},
            {"updated_at": "2026-11-24T08:00:06Z"},
            {"body": duplicate},
            {"body": approved["body"] + "\nextra"},
        ):
            with self.subTest(override=override):
                self.assertIsNone(agent.parse_job(dict(approved, **override), NOW))
        self.assertIsNone(agent.parse_job(approved, NOW + timedelta(hours=24, seconds=1)))
        self.assertIsNone(agent.parse_job(approved, NOW - timedelta(seconds=1)))


class PollTests(TestCase):
    def setUp(self):
        self.ledger = sqlite3.connect(":memory:")
        self.addCleanup(self.ledger.close)
        agent.init_ledger(self.ledger)
        self.device = mock.Mock(return_value="health-observed")

    def poll(self, github, rom_job=None):
        agent.poll(self.ledger, github, 123, self.device, rom_job, clock=lambda: NOW)

    def test_health_uses_simulated_serial_and_publishes_only_summary(self):
        port = mock.Mock()
        port.read.side_effect = [
            b"owner=123456789 AP password=secret sms=private code=123456 token=ghp_example",
            b"Slot A startup checks passed",
        ]
        self.device.side_effect = lambda op, secs: bench.serial_operation(port, op, secs)
        github = FakeGH([request()])
        with mock.patch.object(bench.time, "monotonic", side_effect=[0, 0, 0.5, 1.0]):
            self.poll(github)
        self.device.assert_called_once_with("read-only-health", 1)
        port.write.assert_not_called()
        number, report = github.posts[0]
        self.assertEqual(number, 21)
        self.assertIn("startup success marker", report)
        for secret in ("123456", "secret", "private", "ghp_example"):
            self.assertNotIn(secret, report)

    def test_offline_publication_resumes_without_replaying_device(self):
        github = FakeGH([request(), request(comment_id=42)])
        github.offline = True
        with self.assertRaises(GitHubError):
            self.poll(github)
        github.offline = False
        self.poll(github)
        self.poll(github)
        self.device.assert_called_once()
        self.assertEqual(len(github.posts), 1)

    def test_restart_after_device_started_reports_interrupted(self):
        github = FakeGH([request()])
        self.device.side_effect = SystemExit()
        with self.assertRaises(SystemExit):
            self.poll(github)
        agent.init_ledger(self.ledger)
        self.poll(github)
        self.device.assert_called_once()
        self.assertIn("stopped before recording", github.posts[0][1])

    def test_bad_pr_rejects_permanently_and_untrusted_request_cannot_reserve_uuid(self):
        github = FakeGH([request(maintainer=999), request(comment_id=42)])
        github.eligible = False
        self.poll(github)
        github.eligible = True
        self.poll(github)
        self.device.assert_not_called()
        self.assertFalse(github.posts)
        self.assertEqual(self.ledger.execute("SELECT comment FROM jobs").fetchone(), (42,))

    def test_rom_jobs_require_exact_local_selection(self):
        rom = request(manifest(operation="verify-layout", plan={}), comment_id=42)
        serial = request(manifest(id=SECOND_ID), comment_id=43)
        github = FakeGH([rom])
        self.poll(github)
        self.poll(github, SECOND_ID)
        self.device.assert_not_called()
        github.comments.append(serial)
        self.device.return_value = "layout-ok"
        self.poll(github, ID)
        self.device.assert_called_once_with("verify-layout", 0)

    def test_build_validation_never_uses_device(self):
        job = manifest(operation=agent.BUILD, plan={}, artifact={"id": 52, "sha256": "b" * 64})
        github = FakeGH([request(job)])
        github.archive_matches = False
        self.poll(github)
        self.device.assert_not_called()
        self.assertIn("Requested archive SHA-256", github.posts[0][1])


class DeviceTests(TestCase):
    def test_layout_checks_extent_checksum_and_flags(self):
        # Independently encode the ESP-IDF CSV rows in native binary format.
        csv = Path(__file__).resolve().parents[2] / "partitions.ota-16mb.csv"
        types = {"data": 1, "app": 0}
        subtypes = {"nvs": 2, "phy": 1, "ota_0": 16, "ota_1": 17, "ota": 0}
        rows = [row.split(",") for row in csv.read_text().splitlines() if not row.startswith("#")]
        entries = b""
        for fields in rows:
            label, kind, subtype, offset, size, _ = [field.strip() for field in fields]
            entries += struct.pack(
                "<HBBII16sI", 0x50AA, types[kind], subtypes[subtype], int(offset, 0),
                int(size, 0), label.encode(), 0,
            )
        table = entries + b"\xeb\xeb" + b"\xff" * 14 + hashlib.md5(entries).digest()
        table = table.ljust(0xC00, b"\xff")
        self.assertTrue(bench.layout_matches(table))
        self.assertFalse(bench.layout_matches(table[:-1]))
        for position in (12, 30, 186, 3071):
            changed = bytearray(table)
            changed[position] ^= 1
            self.assertFalse(bench.layout_matches(changed))

    def test_rom_checks_identity_before_flash_and_only_reads_table(self):
        modules = {
            "esptool.cmds": SimpleNamespace(
                detect_flash_size=mock.Mock(return_value="16MB"),
                read_flash=mock.Mock(return_value=b"bad table"),
            ),
            "esptool.targets.esp32s3": mock.MagicMock(),
        }
        loader = modules["esptool.targets.esp32s3"].ESP32S3ROM
        loader.IMAGE_CHIP_ID = 9
        rom = loader.return_value.__enter__.return_value
        rom.sync_stub_detected = False
        rom.get_security_info.return_value = {
            "chip_id": 9, "parsed_flags": {"SECURE_DOWNLOAD_ENABLE": False}, "flash_crypt_cnt": 0,
        }
        with mock.patch.dict(sys.modules, modules):
            rom.read_mac.return_value = bytes(6)
            self.assertEqual(bench.rom_operation(mock.sentinel.serial, "verify-layout"), "identity-mismatch")
            rom.flash_spi_attach.assert_not_called()
            rom.read_mac.return_value = bytes.fromhex(bench.TARGET.replace(":", ""))
            self.assertEqual(bench.rom_operation(mock.sentinel.serial, "verify-layout"), "layout-mismatch")
        rom.connect.assert_called_with(mode="no-reset", attempts=1, detecting=True)
        modules["esptool.cmds"].read_flash.assert_called_once_with(
            rom, 0x8000, 0xC00, flash_size="16MB", no_progress=True,
        )
        rom.run_stub.assert_not_called()
        rom.hard_reset.assert_not_called()

    def test_fault_beats_success_and_idle_is_inconclusive(self):
        for sample, expected in (
            (b"Slot A startup checks passed ... Guru Meditation Error", "health-fault"),
            (b"", "serial-idle"),
            (b"GPS: no fix", "health-inconclusive"),
        ):
            port = mock.Mock()
            port.read.return_value = sample
            with mock.patch.object(bench.time, "monotonic", side_effect=[0, 0, 1]):
                self.assertEqual(bench.serial_operation(port, "read-only-health", 1), expected)

    @mock.patch.object(bench.subprocess, "run")
    def test_worker_enforces_timeout_and_does_not_export_token_env(self, run):
        run.side_effect = bench.subprocess.TimeoutExpired("worker", 6)
        self.assertEqual(bench.run_worker("/dev/ttyACM0", "serial-capture", 1), "device-timeout")
        self.assertEqual(run.call_args.kwargs["timeout"], 6)
        self.assertEqual(set(run.call_args.kwargs["env"]), {"PATH", "HOME"})


class GitHubTests(TestCase):
    def setUp(self):
        self.github = GitHub("never-log-this")
        self.github.request = mock.Mock()

    def test_pr_requires_exact_open_in_repo_head(self):
        pr = {
            "state": "open",
            "base": {"repo": {"id": REPO_ID}, "ref": "main"},
            "head": {"repo": {"id": REPO_ID}, "sha": SHA},
        }
        self.github.request.return_value = (pr, {})
        self.assertTrue(self.github.eligible_pr(21, SHA))
        for field, change in (
            ("head", {"repo": {"id": 999}}), ("head", {"sha": "b" * 40}),
            ("base", {"ref": "other"}), ("base", {"repo": None}),
        ):
            modified = copy.deepcopy(pr)
            modified[field].update(change)
            self.github.request.return_value = (modified, {})
            self.assertFalse(self.github.eligible_pr(21, SHA))
        self.github.request.return_value = (dict(pr, state="closed"), {})
        self.assertFalse(self.github.eligible_pr(21, SHA))

    def test_report_recognizes_own_comment_after_ambiguous_post(self):
        self.github.request.return_value = ([{"body": "receipt", "user": {"id": 123}}], {})
        self.github.report(21, "receipt", 123)
        self.github.request.assert_called_once()
        self.github.request.reset_mock()
        self.github.report(21, "receipt", 999)
        self.assertEqual(self.github.request.call_count, 2)

    def test_archive_checks_commit_digest_and_downloaded_bytes(self):
        content = b"simulated CI artifact archive"
        digest = hashlib.sha256(content).hexdigest()
        artifact = {"id": 52, "sha256": digest}
        metadata = {
            "id": 52, "expired": False, "digest": f"sha256:{digest}", "size_in_bytes": len(content),
            "workflow_run": {"head_sha": SHA, "repository_id": REPO_ID, "head_repository_id": REPO_ID},
        }
        url = "https://artifact.blob.core.windows.net/archive?sig=signed"
        self.github.opener = mock.Mock()
        self.github.request.side_effect = [(metadata, {}), url]
        self.github.opener.open.return_value = BytesIO(content)
        self.assertTrue(self.github.verify_archive(artifact, SHA))
        self.github.opener.open.assert_called_once_with(url, timeout=10)
        self.github.request.side_effect = [(metadata, {}), url]
        self.github.opener.open.return_value = BytesIO(b"substitution")
        self.assertFalse(self.github.verify_archive(artifact, SHA))
        self.github.request.side_effect = [(metadata, {}), "https://example.net/archive"]
        self.assertFalse(self.github.verify_archive(artifact, SHA))
        self.github.request.side_effect = [(metadata, {})]
        self.assertFalse(self.github.verify_archive(artifact, "b" * 40))


class LocalSecurityTests(TestCase):
    def test_private_path_rejects_symlink_and_group_access(self):
        with tempfile.TemporaryDirectory() as root:
            private = Path(root) / "private"
            private.touch(mode=0o600)
            agent.private_path(private)
            link = Path(root) / "link"
            link.symlink_to(private)
            with self.assertRaises(ValueError):
                agent.private_path(link)
            private.chmod(0o640)
            with self.assertRaises(ValueError):
                agent.private_path(private)

    def test_station_lock_prevents_concurrent_execution(self):
        with tempfile.TemporaryDirectory() as root:
            private = Path(root)
            token = private / "token"
            token.touch(mode=0o600)
            token.write_text("never-print-me")
            with (private / "agent.lock").open("a") as lock:
                agent.fcntl.flock(lock.fileno(), agent.fcntl.LOCK_EX | agent.fcntl.LOCK_NB)
                completed = subprocess.run(
                    [
                        sys.executable, "-B", agent.__file__, "--once",
                        "--state-dir", root, "--token-file", str(token),
                    ],
                    capture_output=True, text=True, timeout=5,
                )
            self.assertEqual(completed.returncode, 1)
            self.assertIn("credential and lock", completed.stderr)
            self.assertNotIn("never-print-me", completed.stdout + completed.stderr)
            self.assertFalse((private / "jobs.sqlite3").exists())
