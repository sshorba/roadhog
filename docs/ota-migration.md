# 16 MB A/B OTA baseline and migration

The prototype on `/dev/ttyACM0` reported 16 MB physical quad flash,
manufacturer `1c`, device `7118`, and 3.3 V. The installed ESP32-S3
bootloader header reported 2 MB, DIO, and 80 MHz. The table read at
`0x8000` contained a single 1 MiB factory application. These facts were
checked with esptool 5.6.0. Migration has not been run.

The opt-in `sdkconfig.ota-16mb.defaults` profile sets the 16 MB geometry,
selects `partitions.ota-16mb.csv`, and enables ESP-IDF bootloader rollback.
Use existing DIO and 80 MHz settings. The default single-app build stays
available for the legacy table.

| Region | Offset | Size | Purpose |
| --- | ---: | ---: | --- |
| Bootloader | `0x000000` | up to `0x8000` | Rollback-enabled bootloader |
| Partition table | `0x008000` | `0x1000` | Existing table offset |
| NVS | `0x009000` | `0x6000` | Existing SMS, portal and Wi-Fi data |
| PHY init | `0x00f000` | `0x1000` | Existing PHY range |
| `ota_0` | `0x010000` | `0x400000` | Slot A, at the legacy app offset |
| `ota_1` | `0x410000` | `0x400000` | Slot B |
| `otadata` | `0x810000` | `0x2000` | Two ESP-IDF OTA selection sectors |

Two 4 MiB slots leave room for TLS and telemetry code. The remainder of
the flash is unallocated. Both slots share the existing NVS partition
with its original format and size. There is no separate factory image.
The first boot with erased OTA metadata selects slot A and seeds its
state as valid. Bench-test A before selecting B. Serial ROM download
mode and the backups below are the recovery path for the legacy layout.

## Boot health and rollback

The application evaluates a pending image after ESP-NETIF, the event loop,
the authenticated portal and its NVS storage initialize, the modem UART
opens and drains, the modem returns `OK` to the initial `AT`, SMS setup
succeeds if enabled, and the modem accepts GNSS startup or reports it
already running. This is a startup-level check. It does not require LTE
registration, a GPS fix, live handset delivery, portal client traffic,
or the optional PPP diagnostic.

For `ESP_OTA_IMG_PENDING_VERIFY`, a healthy result calls
`esp_ota_mark_app_valid_cancel_rollback()`. A failed check requests
`esp_ota_mark_app_invalid_rollback_and_reboot()` when ESP-IDF confirms a
rollback image exists. If no suitable fallback exists or an OTA API fails,
the app reports it and avoids deliberately rebooting or marking an
unhealthy image valid. An unconfirmed image can still be aborted by the
bootloader on the next reset, so have serial recovery available. Ordinary
boots of a valid slot do not rewrite OTA metadata.

Host tests exercise the app policy against SDK stubs. The bench-only
controls below exercise the actual bootloader with two serial-staged
images once separately approved. They do not download or install firmware
from the application.

## Build and local checks

Activate ESP-IDF v6.1.1. Run the following from the repository root.
Fresh profiles target ESP32-S3 through `sdkconfig.defaults`.

```sh
idf.py --version
idf.py -B build/ota-normal -D SDKCONFIG=build/ota-normal-sdkconfig \
  -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.ota-16mb.defaults' build
idf.py -B build/ota-lte -D SDKCONFIG=build/ota-lte-sdkconfig \
  -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.lte-diagnostic.defaults;sdkconfig.ota-16mb.defaults' build
sh tests/ota_boot_health/run.sh
python "$IDF_PATH/components/partition_table/gen_esp32part.py" \
  --flash-size 16MB partitions.ota-16mb.csv /tmp/roadhog-ota-layout.bin
```

For bench use, copy the existing private `sdkconfig` and
`build/lte-sdkconfig` to their corresponding `build/ota-*-sdkconfig`
paths before building. Use the same build command with `menuconfig` in
place of `build`. On copied configs, explicitly select 16 MB flash,
**Custom partition table CSV**, filename `partitions.ota-16mb.csv`,
offset `0x8000`, and **Enable app rollback support**. Defaults fill
unset values and do not override saved selections. Check the existing
SMS owner, APN and LED GPIO locally. For the normal bench configuration,
leave the LTE diagnostic disabled. Correctness of a production build
still depends on the owner’s actual hardware settings.

Inspect both builds with `idf.py ... partition-table` or the SDK
`gen_esp32part.py`. Confirm bootloader offset `0`, table offset `0x8000`,
application offset `0x10000`, a 16 MB bootloader header, and both 4 MiB
slots. Each image must fit both. Compare the new and old tables to
confirm exact NVS and PHY offsets and sizes. In local ESP-IDF v6.1.1
builds using the existing bench settings, the normal OTA image was
915,232 bytes and the LTE diagnostic image was 1,046,832 bytes.
Both fit each 4,194,304-byte slot.

## Owner-approved one-time migration

The following commands are for a future approved bench session.
Stop for owner review before **any** write or erase. Use stable power
and disconnect the serial monitor. Do not use a generic flash target
for this migration. Verify the correct board and consult the backup
before proceeding if any observed offsets or security settings differ.
This procedure targets the observed plaintext layout. If secure boot,
flash encryption, or restricted ROM download is enabled, stop and
review a device-specific migration.

### Back up and verify

1. Create a fresh private directory. Back up the entire physical flash
   and the build configs locally. These files contain credentials.

   ```sh
   umask 077
   backup=$(mktemp -d "$HOME/roadhog-legacy.XXXXXX")
   export backup
   python -m esptool --chip esp32s3 -p /dev/ttyACM0 get-security-info
   python -m esptool --chip esp32s3 -p /dev/ttyACM0 --baud 460800 \
     read-flash 0x0 0x1000000 "$backup/flash-16mb.bin"
   cp sdkconfig "$backup/sdkconfig"
   cp build/lte-sdkconfig "$backup/lte-sdkconfig"
   git rev-parse HEAD > "$backup/migration-source-commit"
   ```

2. Extract and inspect the legacy recovery image, table, NVS and future
   OTA metadata. Keep the path and checksum manifest offline.

   ```sh
   python - <<'PY'
   import os
   from pathlib import Path

   backup = Path(os.environ["backup"])
   flash = (backup / "flash-16mb.bin").read_bytes()
   assert len(flash) == 0x1000000, "incomplete backup"
   for name, start, end in (
       ("legacy-bootloader.bin", 0, 0x8000),
       ("legacy-partition-table.bin", 0x8000, 0x9000),
       ("legacy-nvs.bin", 0x9000, 0xf000),
       ("legacy-factory.bin", 0x10000, 0x110000),
       ("pre-migration-otadata.bin", 0x810000, 0x812000),
   ):
       (backup / name).write_bytes(flash[start:end])
   assert flash[0x810000:0x812000] == b"\xff" * 0x2000, "otadata not erased; stop"
   PY
   python "$IDF_PATH/components/partition_table/gen_esp32part.py" \
     "$backup/legacy-partition-table.bin"
   python -m esptool --chip esp32s3 image-info "$backup/legacy-bootloader.bin"
   python -m esptool --chip esp32s3 image-info "$backup/legacy-factory.bin"
   (cd "$backup" && sha256sum ./*.bin sdkconfig lte-sdkconfig migration-source-commit > SHA256SUMS)
   ```

   Require legacy NVS `0x9000/0x6000`, PHY `0xf000/0x1000`, factory
   `0x10000/0x100000`, and a readable legacy bootloader and app.
   If future `otadata` is not blank, stop. Clearing those two sectors
   requires separate explicit approval after saving their contents.

### Write, verify and boot A

3. After explicit approval, put the board in ROM download mode.
   Stage the new baseline application, then write its bootloader and
   partition table. Keep the board in download mode until verification.
   Commands below assume the owner chose the normal OTA build for A.

   ```sh
   ota=build/ota-normal
   python -m esptool --chip esp32s3 -p /dev/ttyACM0 --baud 460800 --after no-reset \
     write-flash 0x10000 "$ota/roadhog.bin"
   python -m esptool --chip esp32s3 -p /dev/ttyACM0 --baud 460800 --after no-reset \
     write-flash --flash-mode keep --flash-freq keep --flash-size keep \
     0x0 "$ota/bootloader/bootloader.bin" 0x8000 "$ota/partition_table/partition-table.bin"
   python -m esptool --chip esp32s3 -p /dev/ttyACM0 --after no-reset verify-flash \
     0x0 "$ota/bootloader/bootloader.bin" \
     0x8000 "$ota/partition_table/partition-table.bin" 0x10000 "$ota/roadhog.bin"
   python -m esptool --chip esp32s3 -p /dev/ttyACM0 --after no-reset \
     read-flash 0x9000 0x6000 "$backup/nvs-before-first-boot.bin"
   cmp "$backup/legacy-nvs.bin" "$backup/nvs-before-first-boot.bin"
   ```

   These writes cover the app, bootloader and partition table only. If
   any write or verification fails, keep or return the board to ROM
   download mode and use the recovery procedure. A power loss between
   writes can leave a nonbooting combination. Do not proceed if NVS
   readback differs.

4. Release BOOT and reset using EN. Open the serial monitor for the OTA
   build. Verify the bootloader selects `ota_0` at `0x10000`. Test
   SMS `STATUS`, `GPS` and an existing additional user, the saved portal
   login after `APON`, and the LED. Use the
   [LTE diagnostic guide](lte-diagnostic.md) for an OTA build with PPP
   enabled. Keep a known-good slot and the legacy backups.

### Exercise confirmation and rejection

Build two local test variants using separate copies of the OTA normal
config. In **Road Hog OTA rollback bench**, enable
`ROADHOG_OTA_BENCH_BOOT_PEER` for the slot A selector. For the forced
failure trial, enable `ROADHOG_OTA_BENCH_REJECT_PENDING` only in the
slot B candidate. For the positive trial, leave both controls disabled
in B. Keep identical settings for the modem, SMS and portal in all images.

```sh
cp build/ota-normal-sdkconfig build/ota-selector-sdkconfig
cp build/ota-normal-sdkconfig build/ota-reject-sdkconfig
for variant in ota-selector ota-reject; do
  idf.py -B "build/$variant" -D "SDKCONFIG=build/$variant-sdkconfig" \
    -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.ota-16mb.defaults' menuconfig
  idf.py -B "build/$variant" -D "SDKCONFIG=build/$variant-sdkconfig" \
    -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.ota-16mb.defaults' build
done
```

Use `build/ota-normal/roadhog.bin` as the healthy B candidate.

5. In a separately approved write session, with A healthy and B untried,
   write the candidate to `0x410000` and the selector to `0x10000`.
   Use `write-flash` and `verify-flash` for those two offsets as above.
   Leave the bootloader, table and NVS alone. Reset the board.
6. A should pass startup and select B through `esp_ota_set_boot_partition()`.
   B should log pending verification. A healthy B should log that it
   marked itself valid and boot again after reset. A forced-fail B
   should log its rollback request, reboot to A, and stay there.
   Verify SMS, GPS, the portal and LEDs again. Use distinct firmware
   roles and capture the slot and health logs with the result.
7. Run the two trials independently. Before a second trial, restore
   the known-good A and stage the next B by approved serial writes.
   To reuse the selector, separately obtain approval to reset only the
   OTA metadata using `erase-region 0x810000 0x2000` in ROM download
   mode. Verify both application files before allowing the first boot
   with blank metadata. Existing metadata otherwise intentionally
   prevents the selector from retrying a rejected or already tried B.

## Recovery to the legacy layout

With separate owner approval, enter ROM download mode. Check the
manifest and restore the backed-up factory app, legacy bootloader and
legacy table. The older table ignores data in the new slots and OTA
metadata, so leave those areas alone during recovery.

```sh
(cd "$backup" && sha256sum -c SHA256SUMS)
python -m esptool --chip esp32s3 -p /dev/ttyACM0 --baud 460800 --after no-reset \
  write-flash 0x10000 "$backup/legacy-factory.bin"
python -m esptool --chip esp32s3 -p /dev/ttyACM0 --baud 460800 --after no-reset \
  write-flash --flash-mode keep --flash-freq keep --flash-size keep \
  0x0 "$backup/legacy-bootloader.bin" 0x8000 "$backup/legacy-partition-table.bin"
python -m esptool --chip esp32s3 -p /dev/ttyACM0 --after no-reset verify-flash \
  0x0 "$backup/legacy-bootloader.bin" 0x8000 "$backup/legacy-partition-table.bin" \
  0x10000 "$backup/legacy-factory.bin"
```

Reset only after verification. Check the factory boot and current NVS
settings. The recovery deliberately leaves NVS at `0x9000` in place.
Restoring `legacy-nvs.bin` is a separate destructive operation that
overwrites newer settings. Require explicit approval and validate the
target and backup before doing so.
