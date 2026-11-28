"""Compile the IDF 6.1.1 OTA decisions against an in-memory flash fixture."""

import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile

TESTS = Path(__file__).resolve().parent
ROOT = TESTS.parent.parent
SDK = Path(sys.argv[1]).resolve()

version = (SDK / "tools/cmake/version.cmake").read_text()
if re.findall(r"^set\(IDF_VERSION_(?:MAJOR|MINOR|PATCH) (\d+)\)$", version, re.M) != ["6", "1", "1"]:
    raise SystemExit("Use the ESP-IDF v6.1.1 source tree")

FUNCTIONS = {
    "bootloader_support/src/bootloader_common_loader.c": (
        "bootloader_common_ota_select_crc", "bootloader_common_ota_select_invalid",
        "bootloader_common_ota_select_valid", "bootloader_common_select_otadata",
        "bootloader_common_get_active_otadata",
    ),
    "bootloader_support/src/bootloader_utility.c": (
        "bootloader_utility_get_selected_boot_partition", "set_actual_ota_seq",
    ),
    "app_update/esp_ota_ops.c": (
        "set_new_state_otadata", "rewrite_ota_seq", "compute_ota_seq_for_target_slot",
        "esp_rewrite_ota_data", "esp_ota_set_boot_partition_internal",
        "esp_ota_set_boot_partition", "esp_ota_get_boot_partition",
        "esp_ota_check_rollback_is_possible", "esp_ota_current_ota_is_workable",
        "esp_ota_mark_app_valid_cancel_rollback", "esp_ota_mark_app_invalid_rollback",
        "esp_ota_mark_app_invalid_rollback_and_reboot", "esp_ota_get_state_partition",
    ),
}
parts = []
for path, names in FUNCTIONS.items():
    source = (SDK / "components" / path).read_text()
    for name in names:
        function = re.search(rf"^[\w *]+\b{re.escape(name)}\([^\n]*\)\n\{{.*?^\}}", source, re.M | re.S)
        if not function:
            raise SystemExit(f"Cannot locate {name} in {path}")
        parts.append(function[0])

with tempfile.TemporaryDirectory() as directory:
    tmp = Path(directory)
    (tmp / "sdk_ota_functions.inc").write_text("\n\n".join(parts))
    cc = shlex.split(os.environ.get("CC", "cc"))
    common = [*cc, "-std=c17", "-Wall", "-Wextra", "-Werror",
              "-DCONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=1",
              "-I", str(TESTS), "-I", str(ROOT / "main"), "-I", str(tmp)]
    for role, config in (("selector", "BOOT_PEER"), ("reject", "REJECT_PENDING")):
        subprocess.run([*common, f"-DCONFIG_ROADHOG_OTA_BENCH_{config}=1",
                        f"-Dota_boot_health_finish={role}_finish",
                        f"-Dota_boot_health_pending={role}_pending",
                        "-c", str(ROOT / "main/ota_boot_health.c"),
                        "-o", str(tmp / f"{role}.o")], check=True)
    executable = tmp / "sdk-lifecycle"
    subprocess.run([*common, str(TESTS / "test_sdk_lifecycle.c"),
                    str(tmp / "selector.o"), str(tmp / "reject.o"),
                    "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
