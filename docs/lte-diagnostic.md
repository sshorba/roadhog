# LTE PPP bench diagnostic

This opt-in build runs one IPv4 data session per boot on the SIM7600NA-H.
It uses the ESP Component Registry release of Espressif esp_modem 2.1.0
and ESP-IDF v6.1.1. The current bench wiring is UART1 TX GPIO17 and RX
GPIO18 at 115200, 8N1, without hardware flow control.

The ESP32 first runs its usual AT/SMS/GNSS loop. At the configured delay
it checks EPS registration and saves the current type and APN of PDP
context 1. If registration or preflight fails, it skips the session for
that boot. The AT driver releases UART1 before esp_modem opens it. The
SIM7600 driver configures an IPv4 context and dials PPP. After successful
IP negotiation, the ESP32 resolves `connectivitycheck.gstatic.com`, synchronizes its clock
over SNTP, and sends a certificate- and hostname-verified HTTPS HEAD
request. Only an HTTP 2xx counts as success.

The setup AP may stay online during the test. Its portal and the LEDs use
the latest AT readings taken before the handoff until polling resumes.
PPP temporarily supplies the uplink and resolver through ESP-NETIF route
priority and per-interface DNS. The HTTPS socket is bound to PPP. The AP
continues to serve its local clients and does not forward their traffic
over LTE.

SMS commands and GPS queries pause while esp_modem owns UART1. SMS sent
during this period may be delayed or lost. The diagnostic stops PPP,
checks command mode, attempts to hang up any suspended data call, and
restores the previous PDP type and APN. Then it releases PPP and UART1.
Road Hog opens its AT driver and checks for a fresh `AT` response before
reapplying SMS notification settings and checking GNSS. If command mode
cannot be confirmed, power-cycle both devices. The fallback portal remains
available.

## Build and test

1. Activate ESP-IDF v6.1.1. From the repository root, use a separate
   bench configuration. The diagnostic defaults enable PPP and TLS date
   checks. They use client-only TLS, the ESP-IDF common CA bundle, and
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
   - `Packet attach: ESP_OK` and `Starting SIM7600 PPP negotiation`
   - `PPP IPv4 acquired` and `DNS resolved connectivitycheck.gstatic.com`
   - `HTTPS HEAD succeeded` with HTTP 2xx and certificate verification
   - `Closing LTE diagnostic session`, `PPP stopped and command mode restored`,
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
- **Timeout or disconnect.** If PPP does not acquire IPv4 in 45 seconds,
  verify that closing begins. On a controllable bench link, interrupt
  data service after negotiation without removing modem power. Check
  the failure and recovery messages and send another `STATUS` once
  registration returns. Packet attach has a 15-second command timeout.
- **DNS, time, or HTTPS error.** On a controllable link, block the
  respective service. Look for the stage-specific failure, PPP cleanup,
  and AT recovery. SNTP has a 15-second wait and HTTPS has a 10-second
  network I/O timeout. A DNS or TLS failure is a failed diagnostic even
  when the modem reports an active data context.
- **Failed AT recovery.** If the link to the modem disappears entirely,
  confirm that the firmware reports that it cannot resume AT. Restore the
  link and power-cycle both devices before sending fresh SMS. The
  application waits at most three minutes for the diagnostic worker and
  will not reopen the UART if the worker exceeds that budget.

Hardware validation is pending. This milestone covers one diagnostic
session on IPv4 with a carrier APN and context 1.
