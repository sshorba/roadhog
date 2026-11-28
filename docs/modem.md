# SIM7600NA-H modem bring-up

The UART settings come from the working bench probe. ESP32-S3 UART1 uses
GPIO17 (TX to modem RX), GPIO18 (RX from modem TX), and 115200 baud, 8N1,
with flow control disabled. Use the existing modem power supply and common
ground.

## Component structure

- `components/modem/include/modem.h` exposes the transport API and result types.
- `components/modem/modem.c` owns the UART driver, parses complete CR/LF-delimited
  lines across reads, and handles one command at a time.
- `main/main.c` contains the board pin assignments and bench probes. Keep future
  modem feature logic above the transport, with board settings in the application.

One FreeRTOS task owns the handle. It calls `modem_command()` for synchronous
transactions and `modem_poll()` to service idle receive. Notification callbacks
run on the owner task during either call. Copy any line you need to retain and
return promptly. A future application with multiple clients can serialize their
requests through the modem owner task.

The component removes command echo, matches solicited payload using the caller's
prefix, and sends other complete lines to the notification callback. If a URC
shares the expected prefix, it is treated as payload. Finals supported by this
milestone are verbose `OK`, `ERROR`, `+CME ERROR:`, and `+CMS ERROR:`. The return
value reports the transport outcome. Check `result.final` for modem acceptance.
Response overflow is reported explicitly, and the parser still consumes the
final. The API covers single-line AT commands with text responses.

AT replies carry no transaction ID. After a timeout the transport keeps the
expired command pending, discards its late echo and matching payload, and
continues routing unrelated notifications. Its eventual final clears the
pending command. Until then new commands report `ESP_ERR_INVALID_STATE` and
nothing is sent. If the final never arrives, reestablish a known idle channel
by restarting both devices. UART overflow and framing faults also require
reinitialization. Command timeouts are measured after sending, with FreeRTOS
tick granularity on UART reads.

## Build and flash

From the repository root with ESP-IDF v6.1.1 activated:

```sh
idf.py --version
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

Use the ESP32's console port in place of `/dev/ttyACM0`. In the local EIM
installation the activation command is
`. "$HOME/.espressif/tools/activate_idf_v6.1.1.sh"`.
Exit the monitor with Ctrl+].

## Bench validation

1. Power the modem and let it boot, then reset the ESP32. Verify the UART1
   startup log and `AT final: OK`.
2. Watch the read-only status probes repeat at roughly five-second intervals.
   `+CPIN: READY` means the SIM is ready. A PIN or PUK response indicates that
   the SIM needs attention. The `LTE:` log summarizes registration, data
   context, and GPS fix status. For `+CSQ:`, 99 means the corresponding signal
   measurement is unavailable. With modem echo enabled, echoed commands
   should stay out of the unsolicited log. These observations verify SIM
   readiness and registration. Data service requires its own test.

3. Trigger a modem notification while the monitor is in the five-second idle
   interval. Use an incoming call to a voice-enabled SIM to produce `RING`,
   or observe startup notices such as `RDY` if your modem firmware emits them
   after the ESP32 has started. Verify an `Unsolicited:` log and continued
   successful `AT+CSQ` polling.
4. For the timeout test, power down the bench setup and disconnect the modem's
   TX-to-ESP32-RX path. Start the ESP32 again. Verify `AT: ESP_ERR_TIMEOUT`
   at roughly two seconds after transmission and subsequent
   `ESP_ERR_INVALID_STATE` reports. Power down, restore the wire, and restart
   both devices. Verify the results in step 1 again.
5. To verify interleaving and late-final handling, substitute a 3.3 V UART
   adapter or scripted modem for the SIM7600 on UART1. Reply to startup probes
   with `OK\r\n` for `AT`. Use the default build with SMS disabled. Reply to
   `AT+CGPS?` with `+CGPS: 1,1\r\nOK\r\n`. Reply to the recurring probes with
   `+CPIN: READY\r\nOK\r\n`, `+CEREG: 0,1\r\nOK\r\n`,
   `+CGACT: 1,0\r\nOK\r\n`, and `+CGPSINFO: ,,,,,,,,\r\nOK\r\n`.
   For the first `AT+CSQ`,
   send `+CS`, then `Q: 15,99\r\nRING\r\n`, then `OK\r\n` within two seconds.
   Verify the payload, separate unsolicited `RING`, and final. For the next
   `AT+CSQ`, delay its reply for three seconds. Verify that the call times out,
   its late payload and final are discarded, and the subsequent poll sends
   a fresh command that completes only after a fresh reply.

Record the modem firmware revision, observed results, and any unexpected
notifications alongside the hardware test.

See the [SIMCom AT command manual](https://download.mikroe.com/documents/datasheets/SIM7600_AT_Commands.pdf) for the full response formats.

## Recorded bench results

On November 20, 2026 the ESP32-S3 and SIM7600NA-H bench setup returned
`AT final: OK`, `+CPIN: READY`, and `+CEREG: 0,1` on repeated polls.
This verified SIM readiness and home LTE registration on that setup.
The modem firmware revision and carrier were unrecorded.

## Automatic SIM7600 recovery

Local startup does not wait for the modem. UART0 diagnostics, status LEDs
and the setup portal start first. The application task is the only AT UART
owner. It waits, initializes, goes online, or recovers with a five-second
pause between failed attempts. The online check uses command responses and
GNSS engine state. LTE searching, denied registration, or lack of a GNSS fix
does not by itself restart the AT session. The portal shows unknown LTE and
unavailable GNSS while recovering. UART1 is released on a fault and reopened
only after any PPP worker has finished its cleanup.

Recovery discards RAM-only direct SMS deliveries and acknowledgements from
the old UART session. It keeps provisioned authorization and reconfigures
PDU mode, SMS routing and GNSS on the new session. Carrier registration and
the reported data context are sampled again. The existing LTE session
code still owns temporary PPP contexts. If SMS OTA support is present,
recovery cancels pending challenges and preselection work, rechecks direct
routing, and resumes the existing ME inbox handling. The durable boot report
remains governed by its existing submission rules.

A pending OTA image has 90 seconds from application startup to complete
the existing portal, AT, SMS and GNSS health checks. The deadline is checked
between bounded initialization attempts and before accepting success. If an
attempt is in flight at the deadline, the decision follows that attempt and
late success cannot mark the image valid. Missing the deadline uses the
existing rollback path. If the SDK cannot roll back, the image remains pending
and modem retries continue. Factory or already validated firmware retries
without a deadline.

### Attended verification

1. After separate hardware approval, start with a provisioned, selected VALID
   image and capture UART0 logs. Leave modem power off at ESP32 startup. Verify
   local portal access, unknown LTE and an offline modem indicator. Power the
   modem and verify the same ESP32 boot reaches online, with SMS commands,
   LTE status and GNSS startup restored.
2. Repeat with modem power applied first and with both supplies applied
   together. During normal operation cycle modem power only. Verify one UART
   owner, unchanged ESP32 uptime and provisioning, and restored SMS and GNSS.
3. With AT still responding, induce loss of registration in a controlled bench
   setup. Verify the portal reports the registration state without repeatedly
   reinitializing the modem. Restore coverage and verify registration resumes.
4. On a separate, approved rollback trial with a verified fallback and backup,
   boot a pending candidate. Verify a modem that becomes ready within the
   health window permits validation. In a separate run hold the modem offline
   beyond the window and verify rollback. Restore modem power and verify the
   previous validated slot recovers without another manual reset.
