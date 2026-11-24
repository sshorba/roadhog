"""Bounded read-only ESP32-S3 worker. Only fixed status keys leave this process."""

import argparse
import hashlib
import struct
import subprocess
import sys
import time


TARGET = "98:c3:77:b4:1a:10"
SERIAL_OPERATIONS = {"serial-capture", "read-only-health"}
ROM_OPERATIONS = {"device-info", "verify-layout"}
RESULTS = {
    "device-ok": "ROM identified the enrolled ESP32-S3 and detected 16 MiB flash.",
    "layout-ok": "The checksummed partition table matches the enrolled A/B layout.",
    "layout-mismatch": "The partition table did not match the enrolled A/B layout.",
    "identity-mismatch": "Device identity could not be verified.",
    "flash-mismatch": "Expected 16 MiB flash could not be verified.",
    "layout-unavailable": "The ROM security state prevents plaintext layout verification.",
    "serial-idle": "No serial bytes arrived during the sample. Result inconclusive.",
    "serial-active": "Serial bytes arrived during the sample. Raw serial was discarded.",
    "health-fault": "A known fault marker was observed in the serial sample.",
    "health-observed": "A known startup success marker was observed with no recognized fault in the sample.",
    "health-inconclusive": "Serial activity was observed without a recognized startup success or fault marker.",
    "device-error": "Device unavailable or the read failed. Check the station and operating mode.",
    "device-timeout": "The device operation exceeded its time limit.",
    "artifact-ok": "The GitHub Actions archive matched the requested SHA-256 and repository commit.",
    "artifact-mismatch": "The archive could not be verified against the requested repository commit and SHA-256.",
    "interrupted": "The station stopped before recording a result. This job will require a new request.",
}

# Mirrored from partitions.ota-16mb.csv, using ESP-IDF binary type/subtype IDs.
PARTITIONS = (
    (0x01, 0x02, 0x9000, 0x6000, b"nvs"),
    (0x01, 0x01, 0xF000, 0x1000, b"phy_init"),
    (0x00, 0x10, 0x10000, 0x400000, b"ota_0"),
    (0x00, 0x11, 0x410000, 0x400000, b"ota_1"),
    (0x01, 0x00, 0x810000, 0x2000, b"otadata"),
)
TABLE_OFFSET = 0x8000
TABLE_SIZE = 0xC00
FAULT_MARKERS = (b"Guru Meditation Error", b"Startup failed", b"UART fault", b"panic'ed")
SUCCESS_MARKERS = (b"marked valid after startup checks", b"Slot A startup checks passed")
SAMPLE_LIMIT = 64 * 1024


def layout_matches(table):
    entries = b"".join(
        struct.pack("<HBBII16sI", 0x50AA, kind, subtype, offset, size, label, 0)
        for kind, subtype, offset, size, label in PARTITIONS
    )
    checksum = b"\xeb\xeb" + b"\xff" * 14 + hashlib.md5(entries, usedforsecurity=False).digest()
    expected = (entries + checksum).ljust(TABLE_SIZE, b"\xff")
    return table == expected


def rom_operation(port, operation):
    from esptool.cmds import detect_flash_size, read_flash
    from esptool.targets.esp32s3 import ESP32S3ROM

    with ESP32S3ROM(port) as rom:
        rom.connect(mode="no-reset", attempts=1, detecting=True)
        if rom.sync_stub_detected:
            return "identity-mismatch"
        security = rom.get_security_info()
        if security["chip_id"] != ESP32S3ROM.IMAGE_CHIP_ID or security["parsed_flags"]["SECURE_DOWNLOAD_ENABLE"]:
            return "identity-mismatch"
        if bytes(rom.read_mac()).hex(":") != TARGET:
            return "identity-mismatch"
        # Set up only the volatile ROM flash interface. No stub or reset.
        rom.flash_spi_attach(0)
        if detect_flash_size(rom) != "16MB":
            return "flash-mismatch"
        if operation == "device-info":
            return "device-ok"
        if security["flash_crypt_cnt"].bit_count() % 2:
            return "layout-unavailable"
        table = read_flash(rom, TABLE_OFFSET, TABLE_SIZE, flash_size="16MB", no_progress=True)
        return "layout-ok" if layout_matches(table) else "layout-mismatch"


def serial_operation(port, operation, seconds):
    deadline = time.monotonic() + seconds
    sample = bytearray()
    while time.monotonic() < deadline and len(sample) < SAMPLE_LIMIT:
        sample.extend(port.read(min(1024, SAMPLE_LIMIT - len(sample))))
    if not sample:
        return "serial-idle"
    if operation == "serial-capture":
        return "serial-active"
    if any(marker in sample for marker in FAULT_MARKERS):
        return "health-fault"
    if any(marker in sample for marker in SUCCESS_MARKERS):
        return "health-observed"
    return "health-inconclusive"


def inspect(port, operation, seconds):
    import contextlib
    import os

    import serial
    from serial.tools.list_ports import comports

    def enrolled_usb():
        return any(
            item.device == port and item.vid == 0x303A and item.pid == 0x1001
            and (item.serial_number or "").lower().replace(":", "") == TARGET.replace(":", "")
            for item in comports()
        )

    try:
        checked_node = os.stat(port)
    except FileNotFoundError:
        return "device-error"
    if not enrolled_usb():
        return "identity-mismatch"
    class BenchSerial(serial.Serial):
        # pyserial 3.5 normally issues separate DTR/RTS ioctls on open.
        # Avoid line transitions that native USB Serial/JTAG uses for reset.
        def _update_dtr_state(self):
            pass

        def _update_rts_state(self):
            pass

    connection = BenchSerial(port=None, baudrate=115200, timeout=0.2, exclusive=True)
    connection.port = port
    with connection, open(os.devnull, "w") as sink, contextlib.redirect_stdout(sink):
        opened_node = os.fstat(connection.fileno())
        if not os.path.samestat(checked_node, opened_node) or not enrolled_usb():
            return "identity-mismatch"
        if operation in ROM_OPERATIONS:
            return rom_operation(connection, operation)
        return serial_operation(connection, operation, seconds)


def run_worker(port, operation, seconds):
    # Isolated Python and a minimal environment keep station credentials and
    # esptool overrides out of the worker. stderr may include raw device data.
    try:
        completed = subprocess.run(
            [sys.executable, "-I", "-B", __file__, port, operation, str(seconds)],
            env={"PATH": "/usr/bin:/bin", "HOME": "/nonexistent"},
            cwd="/", stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
            text=True, timeout=(seconds + 5) if operation in SERIAL_OPERATIONS else 45,
            check=True,
        )
    except subprocess.TimeoutExpired:
        return "device-timeout"
    except (OSError, subprocess.CalledProcessError):
        return "device-error"
    result = completed.stdout.strip()
    return result if result in RESULTS else "device-error"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("port")
    parser.add_argument("operation", choices=sorted(SERIAL_OPERATIONS | ROM_OPERATIONS))
    parser.add_argument("seconds", type=int, choices=range(31))
    args = parser.parse_args()
    try:
        result = inspect(args.port, args.operation, args.seconds)
    except Exception:
        result = "device-error"
    print(result)


if __name__ == "__main__":
    main()
