# Read-only hardware test station

Phase 1 of [Issue #13](https://github.com/sshorba/roadhog/issues/13) uses the
owner's existing Ubuntu workstation as the station for the ESP32-S3/SIM7600NA-H
bench prototype. The station makes outbound HTTPS requests to the GitHub API and,
for build validation, to GitHub Actions archive storage. It has no listener.

## Install on the Ubuntu station

These are manual instructions for the bench operator after reviewing the branch
and approving the host setup. Use Ubuntu with systemd and Python 3.10 or newer.
Run the commands from a reviewed checkout. ESP-IDF is optional for the agent
itself.

1. Install the Python venv package if needed. Create a dedicated local account
   and install a root-owned copy of the reviewed agent.

   ```sh
   sudo apt-get install python3-venv
   sudo useradd --system --user-group --home-dir /var/lib/roadhog-agent \
     --shell /usr/sbin/nologin roadhog-agent
   sudo install -d -m 0755 /opt/roadhog-agent
   sudo install -d -o roadhog-agent -g roadhog-agent -m 0700 /var/lib/roadhog-agent
   sudo install -m 0644 tools/hardware_agent/{agent,bench,github}.py \
     tools/hardware_agent/requirements.txt /opt/roadhog-agent/
   sudo python3 -m venv /opt/roadhog-agent/venv
   sudo /opt/roadhog-agent/venv/bin/python -m pip install -r /opt/roadhog-agent/requirements.txt
   ```

2. Confirm the physical prototype uses the native Espressif USB Serial/JTAG
   interface. Look up its descriptors with
   `udevadm info --attribute-walk --name=/dev/ttyACM0`. Expect VID/PID
   `303a:1001` and an iSerial matching the enrolled MAC
   `98:c3:77:b4:1a:10`. Use the exact iSerial spelling and case reported by
   udev in the following root-owned `/etc/udev/rules.d/99-roadhog-agent.rules`
   example. Stop if the interface or identifier differs.

   ```udev
   SUBSYSTEM=="tty", KERNEL=="ttyACM[0-9]*", ATTRS{idVendor}=="303a", ATTRS{idProduct}=="1001", ATTRS{serial}=="98:C3:77:B4:1A:10", GROUP="roadhog-agent", MODE="0660", ENV{ID_MM_DEVICE_IGNORE}="1"
   ```

   Reload the rules with `sudo udevadm control --reload-rules` and replug the
   USB cable when it is safe to do so. Check `ls -l /dev/ttyACM0`. The
   account needs no broad `dialout` membership. Close IDE monitors, esptool,
   and any other process using the port. If the kernel assigns another
   `ttyACM` number, add `--port /dev/ttyACM1` (or the assigned path) to
   the service and manual commands. Identity is checked for each operation.

3. On GitHub, create a short-lived [fine-grained personal access token](https://docs.github.com/en/authentication/keeping-your-account-and-data-secure/managing-your-personal-access-tokens)
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
     printf "\\n"
     test -n "$token" || exit 1
     printf "%s\\n" "$token" > /var/lib/roadhog-agent/github.token
   '
   sudo chmod 0600 /var/lib/roadhog-agent/github.token
   ```

   Rotate the token by stopping the agent, replacing the file and starting the
   agent again. Revoke the token in GitHub if the station is compromised.

4. Run one poll manually before starting a long-running service. Use the same
   private state directory for every invocation.

   ```sh
   sudo -u roadhog-agent /opt/roadhog-agent/venv/bin/python -E -s -B /opt/roadhog-agent/agent.py \
     --state-dir /var/lib/roadhog-agent --token-file /var/lib/roadhog-agent/github.token --once
   ```

5. When approved, install the example unit and start it manually. The default
   service can run serial and build-validation jobs. Starting it at boot is a
   separate local choice.

   ```sh
   sudo install -m 0644 tools/hardware_agent/roadhog-agent.service /etc/systemd/system/
   sudo systemctl daemon-reload
   sudo systemctl start roadhog-agent
   sudo journalctl -u roadhog-agent -n 30
   ```

   Stop it with `sudo systemctl stop roadhog-agent` or press Ctrl+C for a
   foreground run. If immediate physical isolation is necessary, disconnect USB.
   If boot startup was enabled, use `sudo systemctl disable --now roadhog-agent`
   to prevent a restart at boot. Stop the service before using the device manually.

## Request and review a bench trial

1. Codex Cloud or a maintainer prepares the manifest from an **open same-repository
   PR targeting `main`**. Use its exact current 40-character head SHA and a new
   UUIDv4. Cloud can prepare the request and review results through GitHub. It
   has no direct USB connection to the home station.
2. The enrolled maintainer `sshorba` (GitHub account ID `43084949`) independently
   reviews the exact manifest and posts it as a **new comment on Issue #13** from
   their GitHub account. Put only the fenced block in the comment. PR text, fork
   heads, edits, non-maintainer accounts and comments attributed to a GitHub App
   cannot authorize the station. Replace the example PR, commit and UUID values.

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

3. For the first physical smoke test, leave the ESP32 running its current app,
   start the agent and submit a short `read-only-health` job. Check the PR
   conversation for the job UUID, commit, operation and result. A quiet serial
   sample can be inconclusive. Submit a fresh UUID for a `serial-capture` job to
   check USB serial activity. Confirm the PR report contains no raw output. This
   is the step for the operator to perform on the actual Ubuntu bench.
4. For a separately scheduled ROM check, stop the service and manually put the
   prototype into ROM download mode. Submit a `device-info` or `verify-layout`
   job, then run the station with its exact UUID.

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
| `device-info` | `{}` | `null` | Verifies the native USB descriptor, ROM chip ID, eFuse MAC and 16 MiB JEDEC flash capacity. |
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
  cannot change them. The maintainer authorizes the exact request through an
  independently authored queue comment. Keep the station code and token under
  separate local permissions from contributors. The physical USB interface and
  locally installed software are in the station's trust boundary.
- The running app receives no serial command during capture. The agent
  suppresses pyserial's DTR/RTS ioctls and issues no reset sequence.
  Opening or closing a Linux tty can still change host control lines. Confirm behavior on
  the enrolled bench during the first smoke test. ROM operations send only
  synchronization and read/volatile flash-interface setup commands. They reject
  a resident stub and do not upload a stub or issue a reset. Encrypted layouts
  are unavailable through this plaintext check.
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
