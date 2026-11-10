# Roadmap

Development plan; no functionality has been built or tested yet.

## Phase 1 — Communications prototype
- [ ] Bring up ESP32-S3 + SIM7600NA-H UART and stable power.
- [ ] Test US Mobile activation and LTE registration.
- [ ] Send and receive SMS.
- [ ] Retrieve a validated GNSS fix.
- [ ] Respond to authenticated LOC/STATUS commands with fix age.
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
