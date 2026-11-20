# Roadmap

Development plan and prototype bench progress.

## Phase 1 — Communications prototype
- [x] Verify ESP32-S3 + SIM7600NA-H UART on the bench.
- [ ] Validate stable power under automotive conditions.
- [x] Verify LTE registration on the bench SIM.
- [ ] Verify US Mobile activation and LTE data connectivity.
- [x] Send and receive SMS with the bench STATUS command.
- [ ] Retrieve a validated GNSS fix.
- [ ] Add production command authentication and fix age.
- [ ] Measure idle, GNSS and transmit power draw.

## Phase 2 — Tracking and resilience
- [ ] GPS/geofence algorithm with hysteresis and accuracy checks.
- [ ] Motion detection for parking garages.
- [ ] Persistent location/event log.
- [ ] Detect primary power loss and switch to LiFePO4.
- [ ] Implement low-power, brownout and watchdog behavior.

## Phase 3 — Internet and Home Assistant
- [ ] LTE data simultaneous with SMS/GNSS.
- [ ] MQTT over TLS and Home Assistant device discovery.
- [ ] Wi-Fi STA with multiple SSIDs.
- [ ] Starlink Mini connectivity test.
- [ ] Configurable LTE/Wi-Fi selection and failover.
- [ ] Offline telemetry buffer and reconnection recovery.

## Phase 4 — Configuration and updates
- [ ] First-boot captive portal with authenticated settings.
- [ ] Temporary AP activation through approved methods.
- [ ] Controlled cellular-failure recovery mode.
- [ ] Signed HTTPS OTA with rollback.
- [ ] Secure credential storage and provisioning.

## Phase 5 — Reproducible hardware
- [ ] KiCad reference design, BOM and enclosure.
- [ ] Automotive electrical, thermal, RF and safety validation.
- [ ] Regulatory/carrier compliance review.
- [ ] Assembly and installation documentation.
- [ ] Decide and publish final licenses.

## Future / tertiary features

- Optional hosted MQTT/dashboard subscription.
- VoLTE voice/intercom (conditional on carrier provisioning).
- Protected vehicle accessory interfaces, including compatible remote-start controller triggers.
- DIY kits and fully assembled hardware.
