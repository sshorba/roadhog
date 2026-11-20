# Wi-Fi setup portal

The setup AP starts off while the modem checks LTE registration. When the
modem is registered, the owner can text `APON` to enable it or `APOFF` to
disable it immediately. The AP shuts down after five minutes with no Wi-Fi
stations associated. The idle timer restarts after the last station leaves.

If the modem loses LTE registration or cannot be reached, the AP stays
enabled, even if nobody is connected. During that fallback, `APOFF` does not
disable it. When LTE registration returns, the normal idle timer resumes.
The fallback uses cellular registration and does not depend on an active data
context or internet access.

When enabled, the network is `ROADHOG-XXXX`, using the last four hexadecimal
characters of the Wi-Fi AP MAC address. The network is open and supplies an
IPv4 address, gateway, and DNS server by DHCP.

Join the network on a phone or laptop. Phones that use HTTP captive network
checks should offer to open the setup page. If the prompt does not appear,
open `http://192.168.4.1/` directly. The AP has no internet uplink. The page
is a landing screen for future settings and exposes no configuration controls.

To check the portal from a connected computer:

- Confirm the client received an address in `192.168.4.0/24` and gateway
  `192.168.4.1`.
- Run `nslookup example.com 192.168.4.1` and expect `192.168.4.1`.
- Run `curl -i http://192.168.4.1/generate_204` and expect HTTP 302 with
  `Location: http://192.168.4.1/`. Open the location to see Road Hog Setup.
- Confirm an SMS `STATUS` still works while a client is connected.

To verify the AP policy, start with a registered modem and confirm the SSID
is absent after boot. Send `APON`, join, then disconnect and check that the
SSID disappears after roughly five minutes. Send `APON` again, then
`APOFF` to confirm it can be stopped immediately. With the modem offline,
verify the SSID remains available beyond five minutes.
