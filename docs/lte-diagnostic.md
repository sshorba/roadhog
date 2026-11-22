# LTE PPP bench diagnostic

This opt-in build runs one IPv4 data session per boot on the SIM7600NA-H.
It uses the ESP Component Registry release of Espressif esp_modem 2.1.0
and ESP-IDF v6.1.1. The current bench wiring is UART1 TX GPIO17 and RX
GPIO18 at 115200, 8N1, without hardware flow control.

The ESP32 first runs its usual AT/SMS/GNSS loop. At the configured delay,
it checks EPS registration and requires the last AT probe to finish
before releasing UART1 to esp_modem.
The diagnostic checks packet attachment with `AT+CGATT?`. It never
forces an attach or detach. It uses CID 8 only after the modem reports
no primary or secondary definition and no active bearer for that CID.
An occupied CID, unsupported inventory query, or uncertain response
skips the diagnostic. Existing PDP definitions, including CID 1 and
its carrier-specific fields, are left intact.

The diagnostic defines only the temporary IPv4 CID, then requests PPP
on that CID with `AT+CGDATA`. It starts ESP-NETIF PPP only after a
`CONNECT` result. When IPv4 comes up, it resolves
`connectivitycheck.gstatic.com`, synchronizes its clock over SNTP, and
sends a certificate- and hostname-verified HTTPS HEAD request. Only an
HTTP 2xx counts as success.

The setup AP may stay online during the test. Its portal and the LEDs use
the latest AT readings taken before the handoff until polling resumes.
PPP temporarily supplies the uplink and resolver through ESP-NETIF route
priority and per-interface DNS. The HTTPS socket is bound to PPP. The AP
continues to serve its local clients and does not forward their traffic
over LTE.

SMS commands and GPS queries pause while esp_modem owns UART1. SMS sent
during this period may be delayed or lost. Cleanup runs even if the
modem returns an error or times out while defining the temporary context
or dialing. The diagnostic stops PPP if it attempted to start it, checks
for a command-specific AT reply, and tries a guarded escape if a dial
was attempted and the modem still does not answer. It deactivates only
CID 8 if active, then removes and verifies the temporary definition.
Road Hog opens its AT driver only after cleanup is verified. It checks
for a fresh `AT` response before reapplying SMS notifications and checking
GNSS. If AT or cleanup cannot be verified, the fallback portal stays
available and the serial log calls for a power cycle of both devices.

## Build and test

1. Activate ESP-IDF v6.1.1. From the repository root, use a separate
   bench configuration. The diagnostic defaults enable PPP and TLS date
   checks. They also enable C++ exceptions for esp_modem initialization
   failures and use client-only TLS, the ESP-IDF common CA bundle, and
   size optimization to fit the existing factory partition.

   ```sh
   idf.py --version
   idf.py -B build/lte -D SDKCONFIG=build/lte-sdkconfig \
     -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.lte-diagnostic.defaults' menuconfig
   ```

2. Under **Road Hog LTE diagnostic**, enter the APN for your provisioned
   US Mobile network. The default delay is 90 seconds after startup
   checks. Set up the SMS owner locally as described in
   [SMS commands](sms.md), select the board-specific LED GPIO, and
   match the existing flash configuration. Keep the certificate bundle,
   HTTPS and certificate validity date checks enabled.

   ```sh
   idf.py -B build/lte -D SDKCONFIG=build/lte-sdkconfig \
     -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.lte-diagnostic.defaults' build
   ```

3. With the modem powered and the LTE antenna connected, flash only with
   the bench owner's approval. For an existing compatible Road Hog
   partition table and bootloader, write just the application.

   ```sh
   idf.py -B build/lte -D SDKCONFIG=build/lte-sdkconfig \
     -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.lte-diagnostic.defaults' \
     -p /dev/ttyACM0 app-flash monitor
   ```

4. While the diagnostic countdown is active, send `STATUS` and verify the
   reply. If testing AP coexistence, send `APON` and open the setup portal.
5. Check the monitor for these stages in order. Record the SIM7600
   firmware revision and carrier network with the result.

   - `LTE registration confirmed`
   - `Packet attachment confirmed` and `Starting SIM7600 PPP negotiation`
   - `PPP IPv4 acquired` and `DNS resolved connectivitycheck.gstatic.com`
   - `HTTPS HEAD succeeded` with HTTP 2xx and certificate verification
   - `Closing LTE diagnostic session`, `Temporary PDP context removed and verified`,
     `AT ready after diagnostic`, and `SMS commands resumed`

6. Send a fresh `STATUS` and then `GPS`. Check the replies, portal status
   refresh, and LED indications. Once the diagnostics finish, the firmware
   does not start another session until the next boot.

## Failure checks

- **No SIM or no registration.** With bench power off, remove the SIM or
  otherwise use an unregistered test SIM. Boot and check that the
  diagnostic skips at the deadline and the fallback AP remains available.
- **Wrong APN.** Rebuild with a deliberately invalid test APN. Look for
  attach, dial, or IPv4 failure and the subsequent AT readiness message.
  Verify that a new `STATUS` still works with the registered SIM. Restore
  the correct APN for later tests.
- **Occupied diagnostic CID or unattached packet service.** Check that an
  occupied CID 8 or an unattached modem causes the diagnostic to skip.
  Confirm that SMS resumes on a registered device. Check PDP inventory
  before and after on the bench and confirm the diagnostic has left existing
  definitions untouched.
- **Timeout or disconnect.** Check the 10-second wait for `CONNECT` and
  the 45-second wait for PPP IPv4. On a controllable bench link, interrupt
  data service after negotiation without removing modem power. Check that
  the application verifies temporary CID removal and resumes AT, or
  reports that recovery could not be verified.
- **DNS, time, or HTTPS error.** On a controllable link, block the
  respective service. Expect stage-specific failure messages and
  cleanup. DNS and SNTP waits are 12 and 15 seconds. HTTPS uses the
  DNS result directly, with the original HTTP Host and TLS hostname,
  250 ms transport I/O waits and a 10-second budget checked between
  asynchronous client calls.
- **Failed AT recovery.** If the link to the modem disappears, confirm
  that the firmware reports it cannot resume AT. Restore the link and
  power-cycle both devices. If CID 8 is still defined after a reset,
  inspect it before intentionally removing it. Future diagnostics skip
  an occupied CID.

## Host regression test

After an ESP-IDF build has fetched the pinned esp_modem dependency, run
the AT response tests from the repository root.

```sh
c++ -std=c++20 -Wall -Wextra -Werror \
  -I managed_components/espressif__esp_modem/include \
  components/cellular_net/tests/at_response_test.cpp \
  -o /tmp/roadhog-at-response-test
/tmp/roadhog-at-response-test
```

The fixture follows esp_modem's UART callback contract. Each callback
receives the accumulated response, with partial lines retained until
a newline arrives. The diagnostic processes each complete line once.
An overlong line or an exhausted response buffer fails the transaction
so an incomplete context inventory cannot pass preflight.

## Timeout limits and remaining validation

The AT deadlines bound how long the host waits for a reply. They cannot
cancel an in-progress modem command. A teardown can take longer than
any one AT timeout. esp_modem can wait up to 30 seconds for PPP exit
before trying command mode, and its mode transition and teardown add
their own waits. The HTTPS budget is checked between client calls.
Internal HTTP parsing, event delivery, ESP-NETIF calls and esp_modem
driver calls are subject to SDK scheduling and can overrun their
individual budgets. The application waits at most three minutes for
the worker and does not reopen UART1 if the worker exceeds that time.
It leaves the fallback portal available. The worker keeps ownership
until its own cleanup ends. This avoids a second UART owner while the
SDK is still using the first.

The SIM7600 data-mode transition, carrier support for a temporary IPv4
context, cleanup after a late reply, and actual SMS recovery still
require bench validation. Power loss during the session or an
unresponsive modem can leave a temporary context defined. The next
diagnostic will skip that CID until its state is verified on the bench.

See the [SIMCom SIM7500/SIM7600 AT command manual](https://robu-prod-media.s3.ap-south-1.amazonaws.com/uploads/2022/04/SIM7500_SIM7600_Series_AT_Command_Manual_V3.00.pdf)
for PDP inventory, definition and data mode commands.
