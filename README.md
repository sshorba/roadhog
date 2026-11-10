# Road Hög 🐗

Open-source, self-hostable vehicle tracking and telemetry platform.

> **Status:** Concept and prototype planning. Hardware and features are provisional until tested.

## Goals

- ESP32-S3 with SIM7600NA-H modem for LTE, SMS and GNSS.
- Authenticated SMS location/status commands, geofences, motion alerts and offline logs.
- Self-hosted MQTT and Home Assistant over LTE or Wi-Fi.
- Wi-Fi client connectivity including Starlink Mini, with configurable Wi-Fi/LTE priority.
- Local Wi-Fi configuration portal and signed OTA updates.
- LiFePO4 backup battery and automotive power protection.
- One hardware platform, with SMS-only, self-hosted MQTT, and possible optional paid hosting.
- Open firmware, hardware design, and documentation; no mandatory cloud subscription.

## Prototype candidates

ESP32-S3-DevKitC-1-N16R8, Waveshare SIM7600NA-H, Poynting A-PUCK-0004-V2-01, and approximately 12.8 V 10 Ah LiFePO4 backup power with a BMS and proper charger/power mux.

## Licensing intent

Proposed only (not finalized): GPLv3 firmware, CERN-OHL-S-2.0 hardware and CC BY-SA 4.0 documentation. Commercial reuse intended; product branding remains separate.

## Development direction

Start with SMS, GNSS and power monitoring; then MQTT, Wi-Fi/Starlink failover, Home Assistant, configuration portal and OTA, before custom KiCad hardware.

Vehicle-control functions require dedicated, tested safety safeguards and appropriate automotive interfaces.
