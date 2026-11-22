# Prototype hardware and power notes

**Candidate selections, not a validated production BOM.**

| Subsystem | Candidate | Notes |
| --- | --- | --- |
| Controller | ESP32-S3-DevKitC-1-N16R8 | 16 MB flash / 8 MB PSRAM dev board |
| Cellular/GNSS | Waveshare SIM7600NA-H 4G HAT | UART-controlled SMS, LTE data and GNSS |
| Antenna | Poynting A-PUCK-0004-V2-01 | 2 LTE elements + GNSS; confirm connectors/bias |
| Backup power | ~12.8 V 10 Ah LiFePO4 | 4S pack with BMS and temperature-aware charger |
| Movement | LIS3DH or similar accelerometer | Useful in GPS-denied garages |
| Log storage | Flash or microSD | Sizing/retention TBD |
| Monitoring | Protected vehicle voltage/ignition interfaces | Design for automotive transients |

## Power architecture

```text
Vehicle 12V -> fuse -> reverse-polarity/load-dump protection
                        |
           +------------+------------+
           |                         |
       primary 5V supply       LiFePO4 DC-DC charger
           |                         |
           |                     pack + BMS
           |                         |
           |                   backup DC-DC
           +------------+------------+
                        |
                    power mux
                        |
                  ESP32 + modem
```

Power must transfer without rebooting the modem. Prevent backfeed to the vehicle. Size the modem supply for transmission peaks, not average draw; never feed it from a dev-board 3.3 V output.

The proposed 10 Ah 12.8 V battery stores **128 Wh nominal**; actual runtime depends on measured draw, conversion losses, reserve and environment. LiFePO4 charge limits and vehicle interior temperatures require careful assessment.

## Rural RF considerations

The North American modem and chosen PUCK antenna are intended to include T-Mobile LTE Band 71, along with other US low bands. Antenna positioning, coax losses, ground-plane behavior, GNSS active-antenna bias, connector types, and US Mobile carrier acceptance still need verification.

## Future custom PCB

Plan to publish editable KiCad schematic and PCB, BOM with manufacturer part numbers, assembly files, enclosure models and test procedures. Evaluate ESP32-S3 module temperature grades, automotive EMC/transients, modem regulatory/carrier approvals, and secure manufacturing provisioning.

## Onboard RGB status LED

| Pattern | State |
| --- | --- |
| Solid red | The ESP32 cannot communicate with the modem |
| Flashing red | LTE is unregistered and GPS has a fix |
| Flashing yellow | LTE is registered and GPS has no fix |
| Alternating yellow and red | LTE is unregistered and GPS has no fix |
| Solid green | LTE is registered and GPS has a fix |
| Alternating green and blue | The setup Wi-Fi AP is enabled |

Each step lasts 400 ms. When the AP is enabled during an LTE or GPS fault,
the LED shows two cycles of the fault pattern followed by two cycles of
green and blue. Loss of modem communication always shows solid red.

The default GPIO is 38 for the ESP32-S3-DevKitC-1 v1.1. Set
`ROADHOG_STATUS_LED_GPIO` to 48 for the original revision in `menuconfig`.
See the [Espressif DevKitC-1 guide](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s3/esp32-s3-devkitc-1/user_guide_v1.1.html#hardware-revision-details).
