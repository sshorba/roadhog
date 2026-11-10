# Design decisions and open questions

_Recorded November 10, 2026. Unless indicated otherwise, these are plans rather than implemented features._

## Decisions

1. **Working name:** Road Hög; repository name `roadhog`. Trademark clearance remains outstanding.
2. **Open-source intent:** Publish enough firmware, native PCB design files and docs for independent builds and commercial reuse; branding may be reserved.
3. **Commercial options:** Documentation/DIY, kits, assembled units, and eventually optional paid hosting.
4. **Single hardware configuration:** No Wi-Fi-only SKU. ESP32-S3 + modem + GNSS + Wi-Fi.
5. **Customer connectivity choices:** SMS-only, self-hosted MQTT, and possibly a future managed broker.
6. **Rural coverage:** Focus on LTE low bands, especially Band 71 where T-Mobile is used.
7. **Modem/antenna shortlist:** SIM7600NA-H and Poynting A-PUCK-0004-V2-01.
8. **Backup battery target:** 10 Ah LiFePO4 with an appropriate automotive power system.
9. **Wi-Fi/Starlink:** Multiple SSIDs, Starlink Mini as standard Wi-Fi, user-selectable LTE/Wi-Fi priority, buffered data during outages.
10. **Local portal:** First-boot AP, temporary authenticated activation, carefully limited recovery AP.
11. **Firmware:** ESP-IDF, modular core plus SMS/MQTT adapters; secure OTA.
12. **Ownership:** Device should work without the manufacturer operating a server.

## Open questions

- Exact modem/carrier combinations supporting SMS + data, and potentially VoLTE.
- Continuous SMS availability and actual standby consumption under each connectivity mode.
- GPS and accelerometer power management; geofence false-alarm strategy.
- Battery charging, protection, temperature policy, and measured runtime.
- Storage medium, history retention, MQTT schema and reconnection thresholds.
- Web UI provisioning security and OTA key management.
- Final license texts and contribution policy.
- Production temperature grade and compliance plan.

Please use Issues and pull requests to distinguish **measured results** from hypotheses.
