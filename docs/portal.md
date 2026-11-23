# Wi-Fi setup portal

On a factory device the WPA2 setup AP starts on boot with password
`012345678`, accepts one station, and stays up until a provisioned reboot.
Select **Start setup**, enter the E.164 owner number, carrier APN and a
separate administrator password, and verify the code sent by SMS. When your
own browser prompts you, press the external GPIO4 setup button within 30
seconds. Save the new configured-device Wi-Fi password shown after the
write and restart. See [factory provisioning](factory-provisioning.md).

On a configured device the setup AP starts off while the modem checks LTE registration. When the
modem is registered, the owner can text `APON` to enable it or `APOFF` to
disable it immediately. The AP shuts down after five minutes with no Wi-Fi
stations associated. The idle timer restarts after the last station leaves.

If the modem loses LTE registration or cannot be reached, the AP stays
enabled, even if nobody is connected. During that fallback, `APOFF` does not
disable it. When LTE registration returns, the normal idle timer resumes.
The fallback uses cellular registration and does not depend on an active data
context or internet access.

When enabled, the network is `ROADHOG-XXXX`, using the last four hexadecimal
characters of the Wi-Fi AP MAC address. The configured WPA2 network uses its generated
Wi-Fi credential and supplies an IPv4 address, gateway and DNS by DHCP.

Join the network on a phone or laptop. Phones that use HTTP captive network
checks should offer to open the setup page. If the prompt does not appear,
open `http://192.168.4.1/` directly. The AP has no internet uplink. The page refreshes LTE registration, modem data context, and GPS fix status
every five seconds after sign-in. An active data context does not verify
internet connectivity.

Sign in as `admin` with the password created during setup. Under **Change
password**, enter the current password and confirm the new one. The new
password must contain 12 to 64 UTF-8 bytes. Saving signs you out and stores
a salted password hash in NVS. The portal accepts one session at a time
for up to 30 minutes. HTTP traffic relies on the private Wi-Fi's link
protection. Use the device AP directly and keep its Wi-Fi credential private.

The authenticated dashboard shows the registered owner SMS control number
and a separate SIM phone number when the modem reports one. Missing SIM
number information is shown as “Not available from SIM/carrier.”

To check the portal from a connected computer:

- Confirm the client received an address in `192.168.4.0/24` and gateway
  `192.168.4.1`.
- Run `nslookup example.com 192.168.4.1` and expect `192.168.4.1`.
- Run `curl -i http://192.168.4.1/generate_204` and expect HTTP 302 with
  `Location: http://192.168.4.1/`. Open the location to see Road Hog Setup.
- Run `curl -i http://192.168.4.1/api/status` without a session and expect HTTP
  401. Sign in through the browser and compare the status fields with SMS
  `STATUS`.
- Change the password and confirm the portal signs you out. Try the old
  password, then sign in with the new one. Reboot, enable the AP again,
  and confirm the new password still works.
- Confirm an SMS `STATUS` still works while a client is connected.

To verify the AP policy, start with a registered modem and confirm the SSID
is absent after boot. Send `APON`, join, then disconnect and check that the
SSID disappears after roughly five minutes. Send `APON` again, then
`APOFF` to confirm it can be stopped immediately. With the modem offline,
verify the SSID remains available beyond five minutes.
