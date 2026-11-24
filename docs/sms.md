# Road Hog SMS commands

**Road Hog SMS control** is enabled by default. After authorized [first-time
setup](factory-provisioning.md), restart and wait for `SMS commands enabled`.
The owner is the validated E.164 number in the provisioning record. A factory
device and a device with unreadable provisioning authorize no SMS senders.

Send a command by SMS to the modem SIM. Command names are case-insensitive
and trailing spaces are ignored. Road Hog responds to an authorized sender. An unrecognized command receives
`Unknown command, send "?" for help.`

| Command | Reply |
| --- | --- |
| `?` | Command list |
| `STATUS` | Online confirmation, GPS fix status, LTE registration, and modem data context |
| `GPS` | Fix status, speed in km/h, and signed decimal latitude and longitude when available |
| `MAP` | Google Maps link for a fix or `No GPS` |
| `USERS` | Owner and authorized numbers |
| `ADDUSER +number` | Add a number, owner only |
| `DELUSER +number` | Remove a number, owner only |
| `APON` | Enable the setup AP. The owner receives its SSID and current Wi-Fi password, even if it is already enabled. |
| `APOFF` | Disable the setup AP while LTE is registered, owner only |

Use a single space before the international number. The six additional
numbers persist in the existing `sms/users_v1` NVS record across reboots. SMS sender numbers can be spoofed. Anyone accepted as an
authorized sender can request location and the sender list.

STATUS checks the modem when requested. An active data context indicates a
modem bearer is up. It does not verify internet reachability or activate data.

## Bench checks

- From the owner handset, send mixed-case `sTaTuS` with a trailing space,
  then send `?` and an unknown command. Verify both exact
  replies and confirm `STATUS` still works.
- Check `GPS` and `MAP` before a fix. With the GPS antenna connected and
  under open sky, repeat after a fix and open the returned link.
- Send `ADDUSER +number` for a second handset you control. Confirm `USERS`
  includes it before and after an ESP32 reset. The boot log reports the
  number of additional users loaded from storage. From that handset confirm `STATUS` works
  and user-management commands return `Owner only`. Remove the number from
  the owner handset. Wait for `User removed`, then send a fresh request
  from the removed handset and verify it receives no reply. Reset the ESP32
  and repeat the removal check. The configured owner is permanent.
- The bench firmware handles single-part GSM default-alphabet SMS delivered
  while the controller is online.

GPS command formats come from the [SIMCom AT command manual](https://download.mikroe.com/documents/datasheets/SIM7600_AT_Commands.pdf).
