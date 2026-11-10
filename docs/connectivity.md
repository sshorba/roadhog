# Connectivity and Home Assistant

## Three service models, one hardware platform

| Mode | How it works | Manufacturer subscription |
| --- | --- | --- |
| SMS-only | Text commands and alerts over cellular | None |
| Self-hosted MQTT | Owner's broker and Home Assistant over LTE or Wi-Fi | None |
| Optional managed service (future) | Hosted broker/dashboard | Optional paid service |

Cellular service/SIM costs are separate. SMS should remain available in all modes if the SIM/carrier permit it.

## Wi-Fi and Starlink Mini

**Design decision:** Every Road Hög keeps the LTE/GNSS modem. Wi-Fi is an *additional internet transport*, not a separate hardware SKU. A vehicle-mounted Starlink Mini works as a standard Wi-Fi AP.

- Save multiple trusted SSIDs.
- Owner-configurable Wi-Fi-first or LTE-first priority.
- Verify actual internet or broker reachability; network association alone is insufficient.
- Use reconnect backoff and hysteresis to avoid link flapping.
- Preserve GNSS and log telemetry offline.
- Reconnect MQTT and upload stored data with timestamps and event identifiers after handover.
- Keep the cellular modem reachable for SMS if power use allows.
- Do not rely on Starlink being powered or having an unobstructed satellite view.

## Temporary setup AP

Planned behavior: advertise on first unconfigured startup; otherwise remain off. Authorized requests from a physical setup button, MQTT/Home Assistant, or SMS may activate it for about 15 minutes. Persistent cellular failure may optionally activate a *restricted*, authenticated recovery portal, subject to vehicle state and a configurable timeout. Brief outages should not trigger it.

## MQTT design

Use outbound MQTT over TLS to avoid incoming cellular connections and CGNAT issues. Home Assistant MQTT discovery can expose location, connectivity, battery, movement, ignition, and geofence status. Commands must expire, be authenticated, and carry correlation IDs and acknowledgments.

Illustrative topic names (not yet final):

```text
roadhog/<device-id>/telemetry
roadhog/<device-id>/events
roadhog/<device-id>/availability
roadhog/<device-id>/commands
roadhog/<device-id>/responses
```

## Cellular validation

US Mobile supports network options based on T-Mobile, AT&T, and Verizon. **Frequency support does not guarantee device activation or SMS/VoLTE provisioning.** Validate modem IMEI, SIM, SMS, data, and any voice-calling use before committing to a carrier. Verify simultaneous modem data, GNSS, and SMS operation with the chosen AT/PPP multiplexing approach.

## OTA

Use signed HTTPS firmware downloads, dual OTA partitions, rollback, and battery/vehicle-state prerequisites. Support self-hosted update URLs and prefer Wi-Fi for large updates when practical.
