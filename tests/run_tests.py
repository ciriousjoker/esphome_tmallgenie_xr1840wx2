"""Compile and run host protocol tests using OpenSSL (no ESP32 or credentials)."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PREFIX = os.environ.get("OPENSSL_PREFIX")
if PREFIX:
    crypto_flags = [f"-I{PREFIX}/include", f"-L{PREFIX}/lib", "-lcrypto"]
else:
    try:
        crypto_flags = shlex.split(subprocess.check_output(["pkg-config", "--cflags", "--libs", "openssl"], text=True))
    except (FileNotFoundError, subprocess.CalledProcessError):
        prefix = subprocess.check_output(["brew", "--prefix", "openssl@3"], text=True).strip()
        crypto_flags = [f"-I{prefix}/include", f"-L{prefix}/lib", "-lcrypto"]

with tempfile.TemporaryDirectory(prefix="mesh-pairing-tests-") as folder:
    build = Path(folder)
    headers = ["esp_ble_mesh_ble_api.h", "esp_err.h", "esp_log.h", "esp_random.h", "esp_timer.h", "nvs.h",
               "freertos/FreeRTOS.h", "freertos/queue.h"]
    for name in headers:
        header = build / name
        header.parent.mkdir(parents=True, exist_ok=True)
        header.write_text('#include "platform_stubs.h"\n')
    sources = ROOT / "components/ble_mesh_light"
    common = [os.environ.get("CC", "cc"), "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
              f"-I{build}", f"-I{ROOT / 'tests'}", f"-I{sources}", str(sources / "mesh_pairing_crypto.c"),
              str(ROOT / "tests/crypto_openssl.c")]
    for name in ["crypto", "runtime"]:
        command = common + [str(ROOT / f"tests/test_pairing_{name}.c")]
        if name == "runtime":
            command += [str(sources / "ble_mesh_pairing.c")]
        executable = build / name
        subprocess.run(command + crypto_flags + ["-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True)
