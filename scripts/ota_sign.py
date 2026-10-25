# PlatformIO extra script for signed OTA updates (uses scripts/ota_sign_tool.py).
#
# Before the build: keys/ota_verify_key.bin (the public key the firmware embeds to
# check OTA images) is refreshed from the private signing key, if that key exists.
# After the build: firmware.bin is signed into firmware-signed.bin (ECDSA P-256,
# espsecure "version 1" format). Serial flashing keeps using the plain firmware.bin
# and never needs the key.
#
# Key location: custom_ota_signing_key in platformio.ini, or $ESP32PWM_OTA_KEY.
Import("env")  # noqa: F821 (provided by PlatformIO)

import glob
import os
import subprocess

project_dir = env.subst("$PROJECT_DIR")  # noqa: F821
core_dir = env.subst("$PROJECT_CORE_DIR")  # noqa: F821
build_dir = env.subst("$BUILD_DIR")  # noqa: F821
tool = os.path.join(project_dir, "scripts", "ota_sign_tool.py")

key = os.environ.get("ESP32PWM_OTA_KEY") or env.GetProjectOption("custom_ota_signing_key", "")  # noqa: F821
key = os.path.expanduser(key) if key else ""
verify_key = os.path.join(project_dir, "keys", "ota_verify_key.bin")


def tool_python():
    """An interpreter with the `cryptography` package, or None. ESP-IDF's Python
    environment has it; PlatformIO creates that environment on the first build."""
    candidates = sorted(glob.glob(os.path.join(core_dir, "penv", ".espidf-*", "bin", "python")), reverse=True)
    candidates += [env.subst("$PYTHONEXE"), "python3"]  # noqa: F821
    for exe in candidates:
        try:
            if subprocess.run([exe, "-c", "import cryptography"], capture_output=True).returncode == 0:
                return exe
        except OSError:
            pass
    return None


def run_tool(*args):
    exe = tool_python()
    if not exe:
        return False
    subprocess.run([exe, tool, *args], check=True, stdout=subprocess.DEVNULL)
    return True


def sign(unsigned):
    signed = os.path.join(os.path.dirname(unsigned), "firmware-signed.bin")
    if run_tool("sign", key, unsigned, signed):
        print("ota_sign: wrote %s" % os.path.relpath(signed, project_dir))
    else:
        print("ota_sign: WARNING no Python with 'cryptography' found, firmware-signed.bin not updated")


have_key = bool(key) and os.path.isfile(key)
os.makedirs(os.path.dirname(verify_key), exist_ok=True)
if have_key:
    tmp = verify_key + ".tmp"
    if run_tool("pubkey", key, tmp):
        with open(tmp, "rb") as f:
            public = f.read()
        os.remove(tmp)
        old = open(verify_key, "rb").read() if os.path.isfile(verify_key) else None
        if old != public:
            with open(verify_key, "wb") as f:
                f.write(public)
            print("ota_sign: verification key updated from %s" % key)
    else:
        print("ota_sign: no Python with 'cryptography' yet, keeping keys/ota_verify_key.bin as is")

    # Catch up if an earlier build could not sign (the post action only runs on relink).
    unsigned = os.path.join(build_dir, "firmware.bin")
    signed = os.path.join(build_dir, "firmware-signed.bin")
    if os.path.isfile(unsigned) and (not os.path.isfile(signed) or os.path.getmtime(signed) < os.path.getmtime(unsigned)):
        sign(unsigned)
else:
    print("ota_sign: no signing key at '%s' - firmware-signed.bin will not be produced" % key)
    if not os.path.isfile(verify_key):
        with open(verify_key, "wb") as f:
            f.write(bytes(64))  # all-zero key: the firmware then rejects every OTA image


def sign_firmware(target, source, env):
    if have_key:
        sign(target[0].get_abspath())


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", sign_firmware)  # noqa: F821
