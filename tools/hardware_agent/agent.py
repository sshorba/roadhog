"""Poll maintainer-authorized manifests and publish sanitized read-only results."""

import argparse
from datetime import datetime, timedelta, timezone
import fcntl
import json
import os
from pathlib import Path
import re
import signal
import sqlite3
import stat
import time

from bench import RESULTS, ROM_OPERATIONS, SERIAL_OPERATIONS, TARGET, run_worker
from github import GitHub, GitHubError, REPO


MAINTAINER = 43084949  # sshorba; use a stable GitHub ID, never a display name.
BUILD = "build-validation"
TTL = timedelta(hours=24)
MAX_GITHUB_ID = 2**63 - 1
JOB_ID = re.compile(r"[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}")
HEAD_SHA = re.compile(r"[0-9a-f]{40}")
DIGEST = re.compile(r"[0-9a-f]{64}")
BLOCK = re.compile(r"```roadhog-job\n([^\r]*?)\n```")


def positive_id(value):
    return type(value) is int and 0 < value <= MAX_GITHUB_ID


def matches(pattern, value):
    return isinstance(value, str) and pattern.fullmatch(value) is not None


def unique_fields(pairs):
    result = {}
    for name, value in pairs:
        if name in result:
            raise ValueError("duplicate JSON field")
        result[name] = value
    return result


def parse_job(comment, now):
    user = comment.get("user") or {}
    if user.get("type") != "User" or user.get("id") != MAINTAINER:
        return None
    if comment.get("performed_via_github_app") or not positive_id(comment.get("id")):
        return None
    created = comment.get("created_at")
    if not isinstance(created, str) or created != comment.get("updated_at"):
        return None
    body = comment.get("body")
    if not isinstance(body, str) or len(body) > 2048:
        return None
    block = BLOCK.fullmatch(body.strip())
    if not block:
        return None
    try:
        queued = datetime.strptime(created, "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=timezone.utc)
        job = json.loads(block[1], object_pairs_hook=unique_fields)
    except (ValueError, RecursionError):
        return None
    if not now - TTL <= queued <= now:
        return None
    if type(job) is not dict or set(job) != {
        "version", "id", "pr", "commit", "target", "operation", "plan", "artifact",
    }:
        return None
    if (
        type(job["version"]) is not int or job["version"] != 1
        or not positive_id(job["pr"]) or not matches(JOB_ID, job["id"])
        or not matches(HEAD_SHA, job["commit"]) or job["target"] != TARGET
        or not isinstance(job["operation"], str) or type(job["plan"]) is not dict
    ):
        return None
    operation, plan, artifact = job["operation"], job["plan"], job["artifact"]
    if operation in SERIAL_OPERATIONS:
        if set(plan) == {"seconds"} and type(plan["seconds"]) is int and 1 <= plan["seconds"] <= 30 and artifact is None:
            return job
    elif operation in ROM_OPERATIONS:
        if not plan and artifact is None:
            return job
    elif operation == BUILD:
        if (
            not plan and type(artifact) is dict and set(artifact) == {"id", "sha256"}
            and positive_id(artifact["id"]) and matches(DIGEST, artifact["sha256"])
        ):
            return job
    return None


def report_for(job, result):
    report = (
        f"**Road Hög read-only bench report**\n\n"
        f"- Job `{job['id']}`\n"
        f"- Commit `{job['commit']}`\n"
        f"- Operation `{job['operation']}`\n"
        f"- Result: {RESULTS[result]}\n"
    )
    if job["artifact"]:
        archive_digest = job["artifact"]["sha256"]
        label = "Verified" if result == "artifact-ok" else "Requested"
        report += f"- {label} archive SHA-256 `{archive_digest}`\n"
    return report


def init_ledger(ledger):
    ledger.execute("""
        CREATE TABLE IF NOT EXISTS jobs (
            id TEXT PRIMARY KEY,
            comment INTEGER NOT NULL UNIQUE,
            pr INTEGER NOT NULL,
            state TEXT NOT NULL,
            report TEXT
        )
    """)
    # The hardware action may have started before shutdown. Never run it again.
    with ledger:
        ledger.execute("UPDATE jobs SET state='pending' WHERE state='running'")


def flush_reports(ledger, github, account):
    pending = ledger.execute("SELECT id, pr, report FROM jobs WHERE state='pending' ORDER BY rowid").fetchall()
    for job_id, pr, report in pending:
        github.report(pr, report, account)
        with ledger:
            ledger.execute("UPDATE jobs SET state='sent' WHERE id=?", (job_id,))


def poll(ledger, github, account, device, rom_job=None, clock=lambda: datetime.now(timezone.utc)):
    flush_reports(ledger, github, account)
    since = (clock() - TTL).strftime("%Y-%m-%dT%H:%M:%SZ")
    for comment in github.queue(since):
        job = parse_job(comment, clock())
        if job is None:
            continue
        operation, job_id, pr = job["operation"], job["id"], job["pr"]
        if rom_job is not None and job_id != rom_job:
            continue
        if (operation in ROM_OPERATIONS) != (rom_job is not None):
            continue
        if ledger.execute(
            "SELECT 1 FROM jobs WHERE id=? OR comment=?", (job_id, comment["id"]),
        ).fetchone():
            continue
        if not github.eligible_pr(pr, job["commit"]):
            with ledger:
                ledger.execute("INSERT INTO jobs VALUES (?, ?, ?, 'rejected', NULL)", (job_id, comment["id"], pr))
            print(f"Rejected ineligible PR for job {job_id}", flush=True)
            continue
        if operation == BUILD:
            verified = github.verify_archive(job["artifact"], job["commit"])
            result = "artifact-ok" if verified else "artifact-mismatch"
        else:
            result = "interrupted"
        # Commit before opening the device. If the agent is killed mid-read,
        # the saved report records an interrupted attempt on the next startup.
        with ledger:
            ledger.execute(
                "INSERT INTO jobs VALUES (?, ?, ?, 'running', ?)",
                (job_id, comment["id"], pr, report_for(job, result)),
            )
        if operation != BUILD:
            result = device(operation, job["plan"].get("seconds", 0))
        with ledger:
            ledger.execute(
                "UPDATE jobs SET state='pending', report=? WHERE id=?", (report_for(job, result), job_id),
            )
        print(f"Recorded job {job_id}: {result}", flush=True)
        flush_reports(ledger, github, account)


def private_path(path, *, directory=False):
    info = path.lstat()
    expected = stat.S_ISDIR if directory else stat.S_ISREG
    if not expected(info.st_mode) or info.st_uid != os.geteuid() or info.st_mode & 0o077:
        raise ValueError("Station state and credential must be private and owned by the station user.")


def stop(signum, frame):
    raise SystemExit(128 + signum)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--state-dir", required=True, type=Path)
    parser.add_argument("--token-file", required=True, type=Path)
    parser.add_argument("--port", default="/dev/ttyACM0")
    parser.add_argument("--once", action="store_true")
    parser.add_argument("--rom-job", help="Run only this UUID after manually entering ROM download mode")
    args = parser.parse_args()
    if not re.fullmatch(r"/dev/ttyACM[0-9]+", args.port):
        parser.error("port must be a local native USB ttyACM device")
    if args.rom_job is not None and (not args.once or not matches(JOB_ID, args.rom_job)):
        parser.error("--rom-job requires a job UUID and --once")
    os.umask(0o077)
    signal.signal(signal.SIGTERM, stop)
    try:
        args.state_dir.mkdir(mode=0o700, exist_ok=True)
        private_path(args.state_dir, directory=True)
        private_path(args.token_file)
        with args.token_file.open() as token_file:
            token = token_file.read(1025).strip()
        if not re.fullmatch(r"[A-Za-z0-9_]{1,1024}", token):
            raise ValueError("Invalid token file.")
        with (args.state_dir / "agent.lock").open("a") as lock:
            fcntl.flock(lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
            run_station(args, GitHub(token))
    except (OSError, sqlite3.Error, ValueError):
        parser.exit(1, "Station startup or local state failed. Check private paths, credential and lock.\n")


def run_station(args, github):
    from contextlib import closing

    with closing(sqlite3.connect(args.state_dir / "jobs.sqlite3")) as ledger:
        init_ledger(ledger)
        account = None
        delay = 30
        while True:
            try:
                if account is None:
                    account = github.account()
                poll(
                    ledger, github, account,
                    lambda operation, seconds: run_worker(args.port, operation, seconds),
                    args.rom_job,
                )
            except GitHubError as error:
                print(f"GitHub request failed (HTTP {error.status or 'unavailable'}).", flush=True)
                if args.once:
                    raise SystemExit(1) from None
                # Respect primary rate limits even if periodic requests fail.
                delay = 300 if error.status in {403, 429} else min(delay * 2, 300)
            else:
                if args.once:
                    return
                delay = 30
            time.sleep(delay)


if __name__ == "__main__":
    main()
