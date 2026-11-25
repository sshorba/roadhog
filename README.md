# Road Hög 🐗

Road Hög is an open-source, self-hostable vehicle tracking and telemetry project.
The firmware runs on an ESP32-S3 paired with a SIM7600NA-H cellular modem.
The current prototype uses SMS for remote status and location queries and a local
Wi-Fi portal for setup. It requires no manufacturer cloud account or subscription.
Cellular service requires a compatible SIM and carrier plan.

## What works today

| Capability | Current status |
| --- | --- |
| Modem and SMS | UART communication, LTE registration, and the SMS `STATUS` command have passed bench checks. The provisioned owner can manage additional authorized phone numbers. |
| Location | `GPS` reports fix status, speed, and coordinates when available. `MAP` returns a map link. An open-sky fix and map-link check still need bench validation. |
| Setup and recovery access | The captive portal has passed bench checks. First-time setup verifies ownership by SMS and a physical button press, then stores device settings in flash. On a configured device, the setup access point stays available when LTE registration is lost. |
| Device status | The RGB status LED indicates modem reachability, LTE registration, GPS, and setup access point state. |
| LTE data and firmware updates | The firmware includes an opt-in LTE PPP diagnostic for IPv4, DNS, and HTTPS, plus an opt-in 16 MB A/B partition layout with bootloader rollback. Signed LTE OTA delivery is under development. |

See the [roadmap](ROADMAP.md) for the full v1 checklist and the remaining bench
checks. Geofence alerts, ACC sensing, and guard mode are planned. MQTT, Home
Assistant, Wi-Fi client failover, offline logging, and custom automotive hardware
are later work. Vehicle-control functions that could disable a moving vehicle
require a separate safety review.

## Build the firmware

The bench setup uses an **ESP32-S3-DevKitC-1-N16R8** (16 MB flash, 8 MB PSRAM)
and a **Waveshare SIM7600NA-H**. The modem has its own power supply and shares
ground with the ESP32. Verify the wiring and power requirements before connecting
the boards. [Hardware and power notes](docs/hardware.md) · [Modem wiring and bring-up](docs/modem.md)

With **ESP-IDF v6.1.1** activated, run from the repository root:

```sh
idf.py set-target esp32s3
idf.py build
```

For first boot and owner enrollment, follow the
[factory provisioning guide](docs/factory-provisioning.md). For an existing
prototype, check its partition layout before choosing a flash procedure and keep
a verified recovery backup before migrating it. [A/B layout and recovery guide](docs/ota-migration.md)

## Documentation

| Guide | What it covers |
| --- | --- |
| [Factory provisioning](docs/factory-provisioning.md) | First boot, owner verification, the physical setup button, and saved device settings |
| [SMS commands](docs/sms.md) | Status and location requests, user permissions, and access point control |
| [Wi-Fi setup portal](docs/portal.md) | Login, password changes, captive redirects, and cellular fallback |
| [Prototype hardware](docs/hardware.md) | Candidate parts, wiring, power design, and LED indications |
| [Modem bring-up](docs/modem.md) | SIM7600 wiring, UART settings, integration, and bench checks |
| [LTE diagnostic](docs/lte-diagnostic.md) | Checking PPP, DNS and HTTPS, and restoring modem command mode |
| [A/B OTA migration](docs/ota-migration.md) | Partition layout, backups, boot validation, rollback, and recovery |
| [Roadmap](ROADMAP.md) | What is implemented, what has been bench-tested, and what comes next |
| [Architecture](docs/architecture.md) | Proposed firmware, hardware, and service design |
| [Connectivity and Home Assistant](docs/connectivity.md) | SMS-only use and plans for self-hosted MQTT, Wi-Fi, and optional hosting |
| [Design decisions](docs/decisions.md) | Recorded project decisions and open questions |
| [Development workflow](docs/development-workflow.md) | GitHub issues, pull requests, review, and bench validation |

## Licensing intent

The proposed licenses are GPLv3 for firmware, CERN-OHL-S-2.0 for hardware, and
CC BY-SA 4.0 for documentation. The project intends to allow commercial reuse,
with product branding handled separately. License choices are still pending.
