"""Outbound GitHub API and artifact verification for the enrolled repository."""

import hashlib
from http.client import HTTPException
import json
import time
from urllib.error import HTTPError
from urllib.parse import urlencode, urlsplit
from urllib.request import HTTPRedirectHandler, ProxyHandler, Request, build_opener


REPO = "sshorba/roadhog"
REPO_ID = 1453089052
QUEUE = 13
MAX_ARCHIVE = 64 * 1024 * 1024
MAX_JSON = 8 * 1024 * 1024


class GitHubError(Exception):
    def __init__(self, status=None):
        self.status = status


class NoRedirect(HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


class GitHub:
    def __init__(self, token):
        self.token = token
        self.opener = build_opener(ProxyHandler({}), NoRedirect())

    def request(self, path, data=None, *, archive_redirect=False):
        headers = {
            "Authorization": f"Bearer {self.token}",
            "Accept": "application/vnd.github+json",
            "X-GitHub-Api-Version": "2022-11-28",
        }
        if data is not None:
            headers["Content-Type"] = "application/json"
        request = Request(
            f"https://api.github.com{path}", headers=headers,
            data=None if data is None else json.dumps(data).encode(),
        )
        try:
            with self.opener.open(request, timeout=15) as response:
                raw = bytearray()
                deadline = time.monotonic() + 30
                while chunk := response.read1(64 * 1024):
                    raw.extend(chunk)
                    if len(raw) > MAX_JSON or time.monotonic() >= deadline:
                        raise GitHubError()
                return json.loads(raw), response.headers
        except HTTPError as error:
            with error:
                if archive_redirect and error.code == 302:
                    return error.headers.get("Location")
                raise GitHubError(error.code) from None
        except (OSError, ValueError, HTTPException):
            raise GitHubError() from None

    def pages(self, path, **params):
        for page in range(1, 101):
            query = urlencode(dict(params, per_page=100, page=page))
            items, headers = self.request(f"/repos/{REPO}{path}?{query}")
            yield from items
            if 'rel="next"' not in headers.get("Link", ""):
                return
        raise GitHubError()

    def queue(self, since):
        return self.pages(f"/issues/{QUEUE}/comments", since=since)

    def eligible_pr(self, number, sha):
        try:
            pr, _ = self.request(f"/repos/{REPO}/pulls/{number}")
        except GitHubError as error:
            if error.status == 404:
                return False
            raise
        head = pr.get("head") or {}
        base = pr.get("base") or {}
        return (
            pr["state"] == "open"
            and (head.get("repo") or {}).get("id") == REPO_ID
            and (base.get("repo") or {}).get("id") == REPO_ID
            and base.get("ref") == "main"
            and head.get("sha") == sha
        )

    def account(self):
        user, _ = self.request("/user")
        return user["id"]

    def report(self, number, body, account):
        path = f"/issues/{number}/comments"
        # Recover from a successful POST whose response was lost before the
        # station saved the receipt. Only our authenticated account counts.
        for comment in self.pages(path):
            if comment["body"] == body and (comment.get("user") or {}).get("id") == account:
                return
        self.request(f"/repos/{REPO}{path}", {"body": body})

    def verify_archive(self, artifact, sha):
        path = f"/repos/{REPO}/actions/artifacts/{artifact['id']}"
        try:
            metadata, _ = self.request(path)
        except GitHubError as error:
            if error.status in {404, 410}:
                return False
            raise
        run = metadata.get("workflow_run") or {}
        if (
            metadata.get("id") != artifact["id"]
            or metadata.get("expired") is not False
            or metadata.get("digest") != f"sha256:{artifact['sha256']}"
            or run.get("head_sha") != sha
            or run.get("repository_id") != REPO_ID
            or run.get("head_repository_id") != REPO_ID
            or type(metadata.get("size_in_bytes")) is not int
            or not 0 < metadata["size_in_bytes"] <= MAX_ARCHIVE
        ):
            return False
        try:
            location = self.request(f"{path}/zip", archive_redirect=True)
        except GitHubError as error:
            if error.status in {404, 410}:
                return False
            raise
        if not isinstance(location, str):
            return False
        try:
            url = urlsplit(location)
            port = url.port
        except ValueError:
            return False
        if (
            url.scheme != "https"
            or url.username is not None
            or url.password is not None
            or not url.hostname
            or not url.hostname.endswith((".blob.core.windows.net", ".actions.githubusercontent.com"))
            or port not in {443, None}
        ):
            return False
        digest = hashlib.sha256()
        total = 0
        deadline = time.monotonic() + 90
        try:
            # The signed URL receives no GitHub credential and cannot redirect.
            with self.opener.open(location, timeout=10) as response:
                while chunk := response.read1(64 * 1024):
                    total += len(chunk)
                    if total > MAX_ARCHIVE or time.monotonic() >= deadline:
                        return False
                    digest.update(chunk)
        except (OSError, HTTPException):
            raise GitHubError() from None
        return total > 0 and digest.hexdigest() == artifact["sha256"]
