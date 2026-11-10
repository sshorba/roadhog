# Road Hög — Architecture

**Status: planning; not validated or implemented.**

## Major subsystems

- ESP32-S3 controller with an ESP-IDF-based firmware stack.
- SIM7600NA-H modem for SMS, LTE data, and GNSS, initially over UART.
- Wi-Fi STA for home Wi-Fi, hotspots, and Starlink Mini, plus temporary configuration AP.
- Sensor inputs: vehicle power and ignition, GNSS, accelerometer, battery temperature/energy.
- Local location/event storage and a common authenticated command processor.
- MQTT/Home Assistant integration and independently functioning SMS controls.
- OTA updater with firmware validation and rollback.

## Firmware modules

```text
              Cellular (SMS / GNSS / LTE)    Wi-Fi (STA / setup AP)
                         |                            |
                         +--------- Network Manager --+
                         |               |
                    SMS Adapter      MQTT Adapter
                         \               /
                           Command API
                                |
                        Authorization / audit
                                |
                  GNSS, geofence, monitoring
                                |
                    persistent storage / OTA
```

All transports should use a common command processor. Losing the network must not stop local monitoring or logging. MQTT handover should reconnect cleanly and replay queued, timestamped telemetry.

## Security principles

- Unique device credentials and robust authentication; caller ID by itself is not proof of identity.
- TLS and per-device MQTT credentials, least-privilege ACLs, short-lived non-retained commands.
- Encrypted secrets; evaluate ESP32 secure boot, flash encryption, and signed OTA.
- Authenticated, time-limited setup AP; avoid automatically advertising a recovery AP while armed or moving.
- Rate limits, audit logs and safe behavior after brownouts, resets or reconnects.

Vehicle-accessory commands should be separated from communications logic and use dedicated, properly safeguarded automotive controllers. Remote shutdown of a moving vehicle is outside the supported design.
