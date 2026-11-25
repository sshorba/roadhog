# Agent instructions — Road Hög

These instructions apply to AI coding agents working in this repository. Read the current source and documentation before making changes; documentation may describe plans rather than verified behavior.

## Project and environment

- Road Hög is an open-source ESP32-S3 vehicle tracking, telemetry, and security platform. No mandatory cloud service.
- Primary target: ESP32-S3 N16R8 (16 MB flash, 8 MB PSRAM); ESP-IDF **v6.1.1**; C.
- Bench modem: Waveshare SIM7600NA-H, UART1 TX GPIO17 / RX GPIO18 at 115200 8N1, common ground and independently powered modem.
- Repository references: `README.md`, `ROADMAP.md`, `docs/architecture.md`, `docs/decisions.md`, `docs/modem.md`, `docs/sms.md`, `docs/portal.md`, `docs/connectivity.md`.
- Current firmware implements SMS commands, GNSS queries, modem AT transport, a captive setup portal, and status LEDs. Preserve tested capabilities unless a task explicitly authorizes a change.

## Collaboration workflow (Stage 1: human-triggered Codex CLI)

1. A GitHub issue defines an approved, bounded task with acceptance criteria. **Do not infer authorization to implement unrelated roadmap items.**
2. Work in a dedicated feature branch; do not commit directly to `main`. Reference the issue in commit messages and the pull request.
3. Before coding, inspect the relevant source and describe any architectural trade-offs or safety/security impacts. Ask for clarification if the issue cannot be implemented safely as written.
4. Implement the smallest coherent increment, with focused commits and updated relevant documentation/tests.
5. Build and run available automated tests. Record exact commands and outcomes. Never claim a hardware test passed unless the owner provides the actual result.
6. Open a pull request with a summary, design decisions, test evidence, known limitations, and **step-by-step hardware test instructions**.
7. Wait for human review and physical bench validation before merging, provisioning credentials, changing hardware, or releasing firmware.

For future automation, GitHub issues are the task queue and PRs are the review boundary. Do not treat the existence of an issue as authorization to run autonomous jobs, merge, flash, deploy, or publish releases.

## Engineering rules

- Maintain **single ownership of the modem UART**. Coordinate AT-command responses, unsolicited notifications, and any proposed PPP/CMUX mode changes; avoid independent consumers of the same serial stream.
- Keep network transport separate from application logic; future LTE and Wi-Fi should be usable by shared MQTT/HTTPS/OTA services.
- Preserve SMS recovery and GNSS behavior when changing modem state. Clearly document periods when SMS is unavailable.
- Use supported ESP-IDF APIs and explicit component dependencies. Check compatibility against v6.1.1; do not silently substitute a different framework version.
- Handle retries, partial failures, low memory, brownouts, and modem timeouts without falsely reporting success.
- Never commit SIM data, phone numbers, passwords, tokens, private signing keys, or local configuration. Do not disable TLS verification.
- Cellular registration, data-context activation, and actual IP/HTTPS connectivity are different tests: report them separately.
- Avoid vehicle-control functionality that could disable a moving vehicle. Hardware-dependent automotive functions need explicit safety review.
- For OTA work: use an A/B partition layout, authenticated firmware, rollback, and verified recovery. Treat partition layout migration as a separate, carefully reviewed operation.
- Keep code and docs straightforward. Mark each claim as implemented, bench-tested, or planned as appropriate.

## Local development

With ESP-IDF v6.1.1 activated (installation path may vary):

```sh
idf.py set-target esp32s3
idf.py build
# Hardware only, with owner approval and correct serial port:
# idf.py -p /dev/ttyACM0 flash monitor
```

If prerequisites are unavailable, state the limitation and do not fabricate build or runtime results.
