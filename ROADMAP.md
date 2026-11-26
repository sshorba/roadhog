# Roadmap

v1 is the firmware already in the tree: SMS control of status, location, users, and the setup access point (including the access point that stays up when the modem has no cellular signal), the captive setup portal, the status LED, the opt-in A/B layout, and the opt-in LTE diagnostic. The next features are geofence alerts, relay control, ACC sensing, guard mode, and cop mode. Internet, Home Assistant, signed update delivery, and custom hardware remain later platform work.

A box is checked only when the behavior is in the current firmware. **Bench-tested** means a result is recorded in this repo. A written procedure with no recorded result stays open.

## v1 — MVP

### SMS commands

Text the modem SIM. Command names are case-insensitive. The owner is the provisioned E.164 number. Extra numbers are added with `ADDUSER`. Details and bench steps are in [SMS commands](docs/sms.md).

- [x] `STATUS` — online confirmation, GPS fix status, LTE registration, and modem data context. Bench-tested.
- [x] `GPS` — fix status, speed in km/h, and signed decimal coordinates when a fix is available. A validated open-sky fix is still an open bench check.
- [x] `MAP` — Google Maps link for the current fix (`https://www.google.com/maps/search/?api=1&query=lat,lon`), or `No GPS`. Confirm the link during the GNSS bench check.
- [x] `USERS` — list the owner and authorized numbers.
- [x] `ADDUSER +number` — owner only. The list persists in NVS across reboot.
- [x] `DELUSER +number` — owner only. The configured owner stays on the device.

### Setup access point

The setup AP is the backup path when the device cannot be reached over cellular. Policy is implemented in `main/portal.c` and described in [Wi-Fi setup portal](docs/portal.md).

- [x] `APON` / `APOFF` — turn the setup AP on or off by SMS. An owner `APON` reply includes the SSID and current Wi-Fi password. `APOFF` is owner only.
- [x] On a configured device, loss of LTE registration turns the setup AP on and leaves it on, including when no station is associated. `APOFF` is refused in that state and the reply is `AP stays enabled: cellular unavailable`.
- [x] The fallback follows cellular registration. It does not wait for a data context or for internet access.
- [x] When LTE registration returns, the idle policy resumes: the AP shuts off after five minutes with no associated station.
- [x] Factory first boot starts the setup AP so the owner, APN, and admin password can be claimed before SMS commands are enabled. That path is separate from the no-signal fallback. See [factory provisioning](docs/factory-provisioning.md).
- [x] Captive setup portal with login, live status, and password change. The captive portal path is bench-tested.

### Status LED

- [x] RGB status LED for modem reachability, LTE, GPS, and AP state. Patterns are in [hardware notes](docs/hardware.md).

### OTA baseline

- [x] Opt-in 16 MB A/B layout and bootloader rollback baseline. Migration has not been run on the bench unit. Signed HTTPS download is still planned. See [OTA migration](docs/ota-migration.md).

### LTE diagnostic

- [x] Opt-in LTE PPP diagnostic for an IPv4, DNS, and HTTPS check, separate from SMS. See [LTE diagnostic](docs/lte-diagnostic.md).

v1 still needs these recorded bench checks:

- [ ] Open-sky GNSS fix, then `GPS` and `MAP`. Open the returned link.
- [ ] `USERS`, `ADDUSER`, and `DELUSER` across a reboot. A removed handset gets no reply. The owner remains.
- [ ] `APON` and `APOFF` while the modem is registered. With the modem offline, the SSID stays up past the five-minute idle time, and `APOFF` does not turn it off.

v1 stops at this list. Geofence alerts, relays, ACC sensing, guard mode, cop mode, MQTT, Home Assistant, and signed update delivery come later.

## After v1

Planned. None of this is in the firmware. Command wording is not settled; the behavior below is the requirement.

### Geofencing

Send an SMS alert when the vehicle goes outside a geofence. The check needs hysteresis and a usable accuracy limit so a noisy fix does not repeat the alert. How a fence is defined, stored, and edited, and which numbers receive the alert, is still open.

### Relays

Physical relay outputs, switched by SMS. Each output has a fixed id such as `relay00` or `relay01`, plus an alias. Example: `relay00=fuel pump`. After that, `fuel pump off` and `fuel pump on` switch that relay. The fixed id keeps working after an alias is set.

Put relay control in its own module. One task already owns the modem UART; relay GPIO does not share that stream.

Outputs that can disable a moving vehicle, including a fuel-pump alias, need an explicit safety review before implementation. Remote shutdown of a moving vehicle is outside the supported design until that review changes it. See [architecture](docs/architecture.md). Who may set aliases, and who may switch a relay, is still open.

### ACC voltage

Sense accessory (ACC) voltage and keep the reading as a variable other features can use. Guard mode is the first consumer. [Hardware notes](docs/hardware.md) list a protected vehicle voltage and ignition input as a candidate circuit. It is not designed or built.

### Guard mode

While guard mode is armed, send an SMS alert if the vehicle moves or if ACC becomes active. ACC comes from the voltage variable above. The movement source is still open: GNSS, an accelerometer, or both. A LIS3DH-class accelerometer is a candidate for GPS-denied locations such as a parking garage. It is not selected. How the mode is armed and disarmed, and which numbers are alerted, is still open.

### Cop mode

If the vehicle is stolen, the owner can by SMS name another phone number and an interval. That number then gets regular SMS alerts until the owner stops them. The owner sets the interval. Five minutes is the example, not a fixed period.

The cop-mode number is an alert destination. Command access stays on the v1 user list (`USERS` / `ADDUSER` / `DELUSER`). Alert text is still open. Each interval should at least carry location the way `GPS` or `MAP` does.

## Later platform work

Earlier phase plan. The after-v1 list is the next feature work.

### Power and logging

- [ ] Stable power under automotive conditions.
- [ ] Measure idle, GNSS, and transmit current.
- [ ] Detect loss of primary power and switch to the LiFePO4 pack without rebooting the modem.
- [ ] Low-power, brownout, and watchdog behavior.
- [ ] Persistent location and event log.

### Internet and Home Assistant

- [ ] LTE data while SMS and GNSS still work. One task keeps ownership of the modem UART.
- [ ] US Mobile activation, then a real IP and HTTPS check. Report registration, data-context activation, and reachability as separate results.
- [ ] MQTT over TLS and Home Assistant discovery.
- [ ] Wi-Fi station with more than one saved SSID, including a Starlink Mini.
- [ ] Owner-selectable LTE-first or Wi-Fi-first priority, with failover.
- [ ] Offline telemetry buffer and replay after reconnect.

### Updates and hardware

- [ ] Signed HTTPS OTA on the A/B layout, with rollback and a checked recovery path.
- [ ] KiCad reference design, BOM, and enclosure.
- [ ] Automotive electrical, thermal, RF, and safety validation.
- [ ] Regulatory and carrier review.
- [ ] Assembly and installation documentation.
- [ ] Decide and publish the final licenses.

### Further out

- Optional hosted MQTT and dashboard subscription.
- VoLTE voice or intercom, only where the carrier provisions it.
- Protected accessory interfaces, including a compatible remote-start trigger, after their own safety review.
- DIY kits and assembled hardware.
