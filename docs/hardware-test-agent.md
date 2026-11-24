# Read-only hardware test station

Phase 1 of [Issue #13](https://github.com/sshorba/roadhog/issues/13) uses the
owner's existing Ubuntu workstation as the station for the ESP32-S3/SIM7600NA-H
bench prototype. The station makes outbound HTTPS requests to the GitHub API and,
for build validation, to GitHub Actions archive storage. It has no listener.

## Install on the Ubuntu station

These are manual instructions for the bench operator after approving the host
setup and reviewing the exact checkout commit and installation files. The
dependency locks target Ubuntu x86-64 with CPython 3.10 (Ubuntu 22.04) or 3.12
(Ubuntu 24.04). ESP-IDF is optional for the agent itself.

1. Install the distro's Python venv package if needed. Review the tracked
   agent modules, both dependency locks, the udev rules, and the systemd unit
   before copying files. Use a clean checkout of the approved commit. The
   command sequence assumes a fresh install and an ordinary operator shell.

   ```sh
   sudo apt-get install python3-venv
   sudo useradd --system --user-group --home-dir /var/lib/roadhog-agent \
     --shell /usr/sbin/nologin roadhog-agent
   sudo install -d -m 0755 /opt/roadhog-agent
   sudo install -d -o roadhog-agent -g roadhog-agent -m 0700 /var/lib/roadhog-agent
   sudo install -m 0644 tools/hardware_agent/{agent,bench,github}.py /opt/roadhog-agent/
   sudo install -d -o "$(id -un)" -m 0755 /opt/roadhog-agent/venv
   (
     set -e
     umask 022
     python3 -m venv /opt/roadhog-agent/venv
     python=/opt/roadhog-agent/venv/bin/python
     "$python" -m pip --isolated install --no-cache-dir --index-url https://pypi.org/simple \
       --require-hashes --only-binary=:all: -r tools/hardware_agent/requirements-build.txt
     "$python" -m pip --isolated install --no-cache-dir --index-url https://pypi.org/simple \
       --require-hashes --only-binary=:all: --no-binary=esptool --no-build-isolation \
       -r tools/hardware_agent/requirements.txt
     "$python" -m pip check
   ) && sudo chown -hR root:root /opt/roadhog-agent/venv
   ```

   The runtime lock lists accepted upstream filenames and SHA-256 hashes for
   all direct and transitive dependencies. Esptool 5.2.0 is available as a
   source archive, so it is built locally from the hash-pinned archive using
   the separately hash-pinned build tools. Build isolation is disabled to
   prevent pip from resolving build dependencies separately. All other
   artifacts must be wheels. Run Python, pip and package build hooks as the
   ordinary operator. The privileged commands create directories, copy
   the reviewed files or finalize ownership. Never run pip, setup hooks or
   a checkout-provided installer with sudo. Treat changes to either lock or
   service/rules files as part of the next installation review.

2. On GitHub, create a short-lived [fine-grained personal access token](https://docs.github.com/en/authentication/keeping-your-account-and-data-secure/managing-your-personal-access-tokens)
   for resource owner `sshorba` and select only `roadhog`. Grant **Issues:
   read** for the queue, **Pull requests: read and write** for PR eligibility
   checks and report comments, and **Actions: read** if using build validation.
   GitHub supplies **Metadata: read**. Leave Contents and other permissions
   unset. The poller uses the token's authenticated account ID to recognize its
   own prior reports. Apply any organization approval required by GitHub before
   running the agent.

   Enter the token from a private local terminal. This prompt hides input and
   keeps the value out of process arguments and shell history.

   ```sh
   sudo -u roadhog-agent bash -c '
     umask 077
     read -rsp "GitHub token: " token || exit 1
     printf "\n"
     test -n "$token" || exit 1
     printf "%s\n" "$token" > /var/lib/roadhog-agent/github.token
   '
   sudo chmod 0600 /var/lib/roadhog-agent/github.token
   ```

   Rotate the token by stopping the agent, replacing the file and starting the
   agent again. Revoke the token in GitHub if the station is compromised.

3. Verify credentials before dispatching any jobs. This command makes
   read-only requests for the authenticated account and Issue #13 metadata.
   It never reads queue comments, starts polling, writes reports or opens USB.
   It confirms authentication and connectivity, with no validation of write
   permission for PR reports.

   ```sh
   sudo -u roadhog-agent /opt/roadhog-agent/venv/bin/python -E -s -B /opt/roadhog-agent/agent.py \
     --token-file /var/lib/roadhog-agent/github.token --check-auth
   ```

4. When approved for the bench, confirm descriptors with
   `udevadm info --attribute-walk --name=/dev/ttyACM0`. The observed connection
   uses QinHeng VID/PID `1a86:55d3` and USB serial `5CF9174631`, the default
   enrollment. The alternate native Espressif interface uses VID/PID
   `303a:1001` and an iSerial matching `98:c3:77:b4:1a:10`. Select the latter
   explicitly with `--usb-interface native` in the service and manual dispatch
   commands. Stop if the descriptors differ from the chosen enrollment.

   ```sh
   sudo install -m 0644 tools/hardware_agent/99-roadhog-agent.rules /etc/udev/rules.d/
   sudo udevadm control --reload-rules
   ```

   With operator approval, replug USB at a safe time and confirm the tty
   belongs to group `roadhog-agent` with mode `0660`. The account needs no
   broad `dialout` membership. Close IDE monitors, esptool and other serial
   clients. If the kernel assigns another ttyACM number, use `--port` with
   that exact path in the service and manual commands. The tty path and
   descriptors are checked before and after each open.

5. During the first attended smoke test, activate the reviewed unit with
   explicit operator approval. Review recent Issue #13 queue comments first.
   Starting the service can dispatch eligible jobs already in the queue.
   For a staged station, update the reviewed agent files before activating it.

   ```sh
   sudo install -m 0644 tools/hardware_agent/{agent,bench,github}.py /opt/roadhog-agent/
   sudo install -m 0644 tools/hardware_agent/roadhog-agent.service /etc/systemd/system/
   sudo systemctl daemon-reload
   sudo systemctl enable --now roadhog-agent.service
   systemctl is-enabled roadhog-agent.service
   systemctl is-active roadhog-agent.service
   ```

   Systemd runs the agent and its serial worker as the restricted
   `roadhog-agent` user. Polling, reading the enrolled tty and posting reports
   use the service account's existing permissions without interactive sudo,
   PAM or a fingerprint prompt. Routine requests and results are on GitHub.

   Stop immediately with `sudo systemctl stop roadhog-agent.service`.
   To stop and prevent startup at the next boot, use
   `sudo systemctl disable --now roadhog-agent.service`.
   Press Ctrl+C to stop a foreground run. Disconnect USB if physical isolation
   is necessary. Stop the service before using the device manually.

## Enroll the GitHub connector

Issue #13's REST comments feed exposes the actor under `user` and the App
under `performed_via_github_app`. The reviewed station code pins App ID
`1144995`, slug `chatgpt-codex-connector`, and actor ID `43084949`
(`sshorba`). These values were observed on the owner's original connector
request. For the one-time enrollment, the owner confirms the repository
connection and reviews these values before installing the approved code.
The existing station token and GitHub integration are used as staged.

From an authenticated GitHub CLI, inspect the metadata without posting a job.

```sh
gh api repos/sshorba/roadhog/issues/comments/7125353383 \
  --jq '{actor: (.user | {id, login, type}), app: (.performed_via_github_app | {id, slug})}'
```

GitHub identifies the owner account and App. The agent cannot identify the
person or ChatGPT conversation behind a request made through that account.
The owner should control who can use that connected account. Other GitHub
actors, bots, Apps, and comments with missing or incomplete provenance are
rejected. If GitHub stops returning those fields or replaces the App, use a
direct maintainer comment until a newly observed identity is independently
reviewed and enrolled in the station code. A direct owner comment requires
explicit null App provenance.

## Request and review a bench trial

1. Ask ChatGPT through the connected owner account to run a bounded check on an
   **open same-repository PR targeting `main`**. For example, say “Request a
   10-second Road Hög read-only health check for PR #14.” ChatGPT fetches the
   current exact 40-character head SHA, creates a new UUIDv4, and posts the
   manifest itself as a new comment on Issue #13. The comment contains only
   the fenced manifest. Use the job UUID to find the sanitized result in the
   requesting PR conversation. The connector can request `read-only-health`,
   `serial-capture`, and `build-validation`.
2. Direct owner comments also work. For a manual request, post the block
   below with the current PR number, exact head SHA and a fresh UUID.
   PR text, fork heads and edits cannot authorize a request.

   ````markdown
   ```roadhog-job
   {
     "version": 1,
     "id": "0e11ea1f-89d1-4d23-b7f0-0543bc108ac0",
     "pr": 21,
     "commit": "0123456789abcdef0123456789abcdef01234567",
     "target": "98:c3:77:b4:1a:10",
     "operation": "read-only-health",
     "plan": {"seconds": 20},
     "artifact": null
   }
   ```
   ````

3. For the first physical smoke test, leave the ESP32 running its current app
   and activate the service while the operator is present. Request the short
   health check via ChatGPT and verify the new queue comment and the resulting
   PR report. Match UUID, commit and operation, and check that the report
   contains no raw output. Confirm that opening the tty leaves the app running.
   A quiet sample can be inconclusive. If the app resets or the wrong result
   appears, stop the service and investigate before leaving it unattended.
4. After explicit operator approval for a separate ROM maintenance check,
   stop the service and manually put the prototype into ROM download mode.
   The owner posts a direct `device-info` or `verify-layout` job, then runs the
   station with its exact UUID. Connector comments cannot authorize these
   operations, even if their UUID is selected locally.

   ```sh
   sudo -u roadhog-agent /opt/roadhog-agent/venv/bin/python -E -s -B /opt/roadhog-agent/agent.py \
     --state-dir /var/lib/roadhog-agent --token-file /var/lib/roadhog-agent/github.token \
     --once --rom-job YOUR_JOB_UUID
   ```

   Confirm the report on the requesting PR. When the operator is ready, manually
   return the device to normal boot and resume the service. ROM checks require a
   bench maintenance window because entering ROM interrupts the running app.

| Operation | Plan | Artifact | Checks |
| --- | --- | --- | --- |
| `read-only-health` | `{"seconds": 1}` through 30 seconds | `null` | Samples up to 64 KiB of serial for fixed startup or fault markers. |
| `serial-capture` | `{"seconds": 1}` through 30 seconds | `null` | Reports whether serial activity was seen. |
| `device-info` | `{}` | `null` | Verifies the enrolled USB interface, ROM chip ID, eFuse MAC, 16 MiB JEDEC flash capacity and expected security baseline. |
| `verify-layout` | `{}` | `null` | Performs device-info checks and reads only the 3 KiB partition table at `0x8000`. Verifies the full checksummed table against the A/B layout. |
| `build-validation` | `{}` | `{"id": 123, "sha256": "<64 lowercase hex characters>"}` | Streams and hashes the GitHub Actions artifact ZIP and checks GitHub's digest, artifact ID and originating repository and head SHA. |

For build validation, use an existing, reviewed Actions run with a downloadable
archive. Read its artifact ID and `sha256:` digest from the Actions API, strip
the `sha256:` prefix, and check the workflow run's head SHA against the PR.
Archives larger than 64 MiB or expired archives are rejected. The archive is
never unpacked, executed, stored or sent to the device. This step verifies the
archive and commit association. Build execution and workflow review happen in CI
or on the development host. This change adds no firmware build workflow.

## Trust, recovery and limits

- The allowlist, repository ID, base branch, target MAC, operations, and
  permissible flash read range are in the reviewed station code. GitHub content
  cannot change them. Direct owner comments authorize jobs, and the pinned
  connector can submit only the three ordinary operations on the owner's
  behalf. Keep the station code and token under
  separate local permissions from contributors. The physical USB interface and
  locally installed software are in the station's trust boundary.
- The QinHeng USB descriptor identifies the enrolled adapter as reported by
  the host. Its serial does not identify the downstream ESP32. The native
  iSerial is also a USB descriptor, not cryptographic hardware authentication.
  Both passive operations report UART observations only. They send no app or
  ROM commands, suppress pyserial DTR/RTS ioctls and issue no reset sequence.
  The Linux tty driver can still change lines on open or close. Verify the
  behavior during an approved, attended passive smoke test before enabling
  unattended operation.
- ROM inspection separately checks the ESP32-S3 chip ID and eFuse MAC and
  expects 16 MiB flash, unrestricted ROM download, secure boot disabled, and
  flash encryption disabled, matching the documented plaintext bench profile.
  An unexpected security state stops inspection. ROM operations send only
  synchronization, reads, and volatile flash-interface setup commands. They
  reject a resident stub and neither upload a stub nor issue a reset.
- A report contains only schema-constrained job metadata and a fixed result. It
  cannot include SIM/SMS contents, owner numbers, AP passwords, verification
  codes, tokens, arbitrary tool output, or NVS bytes. Serial buffers, partition
  reads and archive bytes are discarded when the operation ends. The private
  SQLite ledger retains UUIDs and sanitized reports to prevent replay. Preserve
  it across restarts and upgrades. Keep Ubuntu's clock synchronized for the
  24-hour queue window.
- Use one station and one state directory for this prototype. A local file lock
  serializes invocations and the serial connection is opened in exclusive mode.
  The station polls every 30 seconds and backs off after GitHub errors. It
  retries posting saved results after connectivity returns and looks for its
  previous comment before retrying an ambiguous POST. If stopped mid-operation,
  the station uploads an interrupted result on restart and never reruns that
  UUID. Requests that expire while the station is offline need a fresh UUID.
  Resolve authentication or PR comment permission errors before adding new jobs
  because pending results are published first.
- Samples cover only the observed serial window. They don't verify LTE
  registration, portal access, active OTA slot, OTA rollback, or a running image
  SHA. The commit in a hardware report identifies the request, not the firmware
  proven to be running. A layout mismatch can indicate a legacy or different
  partition table. Leave it in place until the operator reviews it.
- This station does not create or restore recovery backups, read app slots,
  protected NVS or OTA metadata, stage a flash operation, or write flash, eFuses,
  or modem state. Review the separate [OTA migration and recovery notes](ota-migration.md)
  for authorized backup and restore work. Keep any recovery files offline and
  private.

## Simulated verification

Run from the checkout with Python 3.10+ and no USB or GitHub credentials.

```sh
python3 -B -m unittest discover -s tests/hardware_agent -v
```
