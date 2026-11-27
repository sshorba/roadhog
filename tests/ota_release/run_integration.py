"""Run the production installer with loopback TLS and simulated ESP32 flash."""

from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import hashlib
import os
from pathlib import Path
import re
import shlex
import ssl
import struct
import subprocess
import sys
import tempfile
import threading

ROOT = Path(__file__).resolve().parents[2]
TESTS = Path(__file__).resolve().parent
PROJECT = "/sshorba/roadhog/releases/download/v2/roadhog-v2."
ASSET = "/github-production-release-asset/roadhog-v2."
S3 = "release-assets.githubusercontent.com"
IMAGE_SIZE = 10003

CASES = (
    "success", "success-from-b", "chunked-image", "dry-run", "dry-run-repeated", "bad-signature", "altered", "truncated",
    "http-failure", "at-failure", "pdp-cleanup-failure", "pdp-cleanup-deferred", "uart-reopen-failure",
    "session-timeout", "task-allocation-failure", "sms-failure", "gnss-failure", "wrong-cert",
    "untrusted-cert", "redirect-http", "redirect-host", "redirect-loop",
    "duplicate-location", "invalid-image", "readback-digest", "link-down", "missing-slot",
    "factory", "missing-apn", "flash-erase", "flash-partial-write", "selection-metadata",
    "sms-pdu", "sms-success", "sms-cancel", "sms-expiry", "sms-replay", "sms-missing", "sms-invalid",
    "sms-malformed-pointer", "sms-equal", "sms-older", "sms-changed", "sms-reply-failed",
    "sms-ack-failed", "sms-journal-failed", "sms-selection-failed", "sms-rollback",
    "sms-report-failed", "sms-report-commit-failed", "sms-owner-changed", "sms-readback",
    "sms-incomplete-expiry", "sms-confirm-reply-failed", "sms-queued-expiry", "sms-reboot-replay",
    "sms-boundary-cancel", "sms-recovery-cancel", "sms-recovery-ack-failed", "sms-quiesce-failed", "sms-resume-failed",
    "sms-report-busy", "sms-nvs-set-failed", "sms-nvs-erase-failed", "sms-reset-before-selection",
    "sms-reset-before-send", "sms-latest-redirect", "sms-latest-wrong-cert", "sms-wrong-sequence",
    "sms-wrong-target", "sms-legacy", "sms-incompatible",
)


def run(*args, **kwargs):
    subprocess.run(args, check=True, **kwargs)


def openssl(*args):
    run("openssl", *args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def compile_fixture(tmp, sdk):
    version = (sdk / "tools/cmake/version.cmake").read_text()
    if re.findall(r"^set\(IDF_VERSION_(?:MAJOR|MINOR|PATCH) (\d+)\)$", version, re.M) != ["6", "1", "1"]:
        raise SystemExit("Use the ESP-IDF v6.1.1 source tree")
    ota_ops = (sdk / "components/app_update/esp_ota_ops.c").read_text()
    sdk_functions = []
    for name in ("esp_ota_resume", "esp_ota_write"):
        function = re.search(rf"^esp_err_t {name}\([^\n]*\)\n\{{.*?^\}}", ota_ops, re.M | re.S)
        if not function:
            raise SystemExit(f"Cannot locate SDK staging function {name}")
        sdk_functions.append(function[0])
    (tmp / "sdk_staging.inc").write_text("\n\n".join(sdk_functions))

    # Compile the full modules with host adapters replacing ESP-IDF includes.
    for source, destination, guard in (
        ("ota_update.c", "installer.inc", "CONFIG_ROADHOG_OTA_UPDATE_BENCH"),
        ("ota_report.c", "report.inc", r"UNUSED_GUARD"),
        ("lte_diagnostic.c", "lte_wrapper.inc", "CONFIG_ROADHOG_LTE_DIAGNOSTIC"),
    ):
        module = (ROOT / "main" / source).read_text()
        outer = ("#if defined(CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE) && "
                 "defined(CONFIG_ROADHOG_SMS_CONTROL)" if source == "ota_report.c"
                 else f"#ifdef {guard}")
        module = module.replace(outer + "\n", "", 1)
        module, closing, tail = module.rpartition("#endif")
        assert closing and not tail.strip()
        module = re.sub(r'^#include "[^"]+"\n', "", module, flags=re.M)
        (tmp / destination).write_text(module)
    sms = (ROOT / "main/sms_control.c").read_text()
    parser = sms.split("static int nibble(", 1)[1].split("\n#endif", 1)[0]
    delivery = sms[sms.index("void sms_control_expire_pdu("):]
    (tmp / "sms_delivery.inc").write_text("static int nibble(" + parser + "\n" + delivery)
    (tmp / "key.S").write_text(
        '.section .rodata\n.global _binary_ota_public_pem_start, _binary_ota_public_pem_end\n'
        '_binary_ota_public_pem_start:\n.incbin "release-public.pem"\n.byte 0\n'
        '_binary_ota_public_pem_end:\n.section .note.GNU-stack,"",@progbits\n'
    )
    flags = shlex.split(subprocess.check_output(
        ["pkg-config", "--cflags", "--libs", "libcurl", "openssl"], text=True
    ))
    binary = tmp / "test_install"
    run(*shlex.split(os.environ.get("CC", "cc")), "-std=gnu11", "-Wall", "-Wextra", "-Werror",
        "-I", str(tmp), "-I", str(ROOT / "main"), ROOT / "main/ota_manifest.c", ROOT / "main/ota_sms.c",
        TESTS / "test_install.c", "key.S", *flags, "-o", binary, cwd=tmp)
    return binary


def make_assets(tmp):
    image = bytearray(((index * 17 + 71) % 256 for index in range(IMAGE_SIZE)))
    image[:288] = bytes(288)
    image[0] = 0xE9
    image[3] = 0x40
    struct.pack_into("<H", image, 12, 9)
    struct.pack_into("<I", image, 32, 0xABCD5432)
    image[48] = ord("2")
    image[80:87] = b"roadhog"
    binary = tmp / "roadhog-v2.bin"
    marker = b"ROADHOG-SMS-OTA1\0"
    notes = "https://github.com/sshorba/roadhog/releases/tag/v2"
    binary.write_bytes(image)
    publisher = ROOT / "tools/ota_release_manifest.py"
    legacy = subprocess.run([sys.executable, publisher, "--sms", binary, "2", notes],
                            capture_output=True)
    assert legacy.returncode and b"lacks the SMS OTA boot capability marker" in legacy.stderr
    incompatible = bytes(image)
    image[288:288 + len(marker)] = marker
    binary.write_bytes(image)
    run(sys.executable, publisher, "--sms", binary, "2", notes)
    openssl("genpkey", "-algorithm", "EC", "-pkeyopt", "ec_paramgen_curve:P-256", "-out",
            tmp / "release-private.pem")
    openssl("pkey", "-in", tmp / "release-private.pem", "-pubout", "-out", tmp / "release-public.pem")
    def signed(name, manifest):
        path = tmp / f"{name}.manifest"
        path.write_bytes(manifest)
        signature = tmp / f"{name}.sig"
        openssl("dgst", "-sha256", "-sign", tmp / "release-private.pem", "-out", signature, path)
        return {"manifest": manifest, "sig": signature.read_bytes()}

    original = binary.with_suffix(".manifest").read_bytes()
    assets = {
        name: signed(name, manifest)
        for name, manifest in {
            "original": original,
            "changed": re.sub(rb"sha256=[0-9a-f]{64}", b"sha256=" + b"a" * 64, original),
            "sequence": original.replace(b"sequence=2", b"sequence=3"),
            "target": original.replace(b"esp32s3", b"esp32c3"),
            "legacy": original.replace(b"roadhog-sms-release-v1", b"roadhog-release-v1"),
            "incompatible": re.sub(rb"sha256=[0-9a-f]{64}",
                                   b"sha256=" + hashlib.sha256(incompatible).hexdigest().encode(),
                                   original),
        }.items()
    }
    for name, release in assets.items():
        release["bin"] = incompatible if name == "incompatible" else bytes(image)
    return binary, assets


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def redirect(self, *locations):
        self.send_response(302)
        for location in locations:
            self.send_header("Location", location)
        self.send_header("Content-Length", "0")
        self.send_header("Connection", "close")
        self.end_headers()

    def do_GET(self):
        case = self.server.scenario
        host = self.headers["Host"]
        if self.connection.fixture_sni != host:
            self.send_error(421)
            return
        if (host == "github.com" and self.path == PROJECT.split("/v2/")[0] + "/latest" or
                host == S3 and self.path == "/latest"):
            if host == "github.com" and case in ("sms-latest-redirect", "sms-latest-wrong-cert"):
                redirect_host = S3 if case == "sms-latest-redirect" else "wrong.example"
                self.redirect(f"https://{redirect_host}/latest")
                return
            if case == "sms-missing":
                self.send_error(404)
                return
            variant = {"sms-changed": "changed", "sms-incompatible": "incompatible",
                       "sms-legacy": "legacy"}.get(case, "original")
            release = self.server.assets[variant]
            data = bytes([len(release["sig"])]) + release["sig"] + release["manifest"]
            if case == "sms-invalid":
                data = data[:-2] + b"x\n"
            elif case == "sms-malformed-pointer":
                data = b"\xff" + data[1:]
            self.send_response(200)
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(data)
            return
        if host == "github.com" and self.path.startswith(PROJECT):
            suffix = self.path.removeprefix(PROJECT)
            if suffix not in self.server.assets["original"]:
                self.send_error(404)
                return
            location = f"https://{S3}{ASSET}{suffix}?sig=fixture%2Bvalue"
            if case == "redirect-http":
                location = location.replace("https:", "http:")
            elif case == "redirect-host":
                location = location.replace(S3, "unlisted.example")
            elif case == "wrong-cert":
                location = location.replace(S3, "wrong.example")
            elif case == "redirect-loop":
                location = "https://github.com" + self.path
            elif case == "duplicate-location":
                self.redirect(location, location)
                return
            self.redirect(location)
            return

        if host != S3 or not self.path.startswith(ASSET):
            self.send_error(400)
            return
        suffix, query = self.path.removeprefix(ASSET).split("?", 1)
        if query != "sig=fixture%2Bvalue":
            self.send_error(400)
            return
        variant = {"sms-wrong-sequence": "sequence", "sms-wrong-target": "target",
                   "sms-incompatible": "incompatible"}.get(case, "original")
        if not case.startswith("sms-") or case == "sms-failure":
            variant = "legacy"
        data = self.server.assets[variant][suffix]
        if case == "http-failure" and suffix == "bin":
            self.send_error(503)
            return
        if case == "bad-signature" and suffix == "sig":
            data = data[:-1] + bytes([data[-1] ^ 1])
        if case == "altered" and suffix == "bin":
            data = data[:-1] + bytes([data[-1] ^ 1])
        self.send_response(200)
        self.send_header("Connection", "close")
        if case == "chunked-image" and suffix == "bin":
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            for offset in range(0, len(data), 513):
                chunk = data[offset:offset + 513]
                self.wfile.write(f"{len(chunk):x}\r\n".encode() + chunk + b"\r\n")
            self.wfile.write(b"0\r\n\r\n")
            return
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        if case == "truncated" and suffix == "bin":
            data = data[:len(data) // 2]
        self.wfile.write(data)


def make_certificate(tmp, name):
    cert = tmp / f"{name}.crt"
    key = tmp / f"{name}.key"
    openssl("req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
            "-subj", f"/CN={name}", "-addext", f"subjectAltName=DNS:github.com,DNS:{S3}",
            "-out", cert, "-keyout", key)
    return cert, key


def main():
    sdk = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory() as directory:
        tmp = Path(directory)
        image, assets = make_assets(tmp)
        runner = compile_fixture(tmp, sdk)
        ca, key = make_certificate(tmp, "loopback")
        unrelated_ca, _ = make_certificate(tmp, "unrelated")
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(ca, key)

        def record_sni(connection, server_name, _context):
            connection.fixture_sni = server_name

        context.set_servername_callback(record_sni)
        with ThreadingHTTPServer(("127.0.0.1", 0), Handler) as server:
            server.socket = context.wrap_socket(server.socket, server_side=True)
            server.assets = assets
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            try:
                for case in CASES:
                    server.scenario = case
                    run(runner, case, image, env={
                        **os.environ, "OTA_TEST_PORT": str(server.server_port),
                        "OTA_TEST_CA": str(unrelated_ca if case == "untrusted-cert" else ca),
                    })
            finally:
                server.shutdown()
                thread.join()


if __name__ == "__main__":
    main()
