#!/usr/bin/env python3
"""OTA signing helper (needs the `cryptography` package, e.g. ESP-IDF's Python env).

Produces the same format as `espsecure.py sign_data --version 1`, which the firmware
checks before installing an OTA image: the image, then uint32 0, then the ECDSA P-256
signature r||s (big-endian, 32 bytes each) over SHA-256 of the image.

    ota_sign_tool.py genkey KEY.pem            create a new private key (never overwrites)
    ota_sign_tool.py pubkey KEY.pem OUT.bin    64-byte X||Y public key embedded in the firmware
    ota_sign_tool.py sign KEY.pem IN.bin OUT.bin
    ota_sign_tool.py verify PUB.bin SIGNED.bin
"""
import os
import struct
import sys

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec, utils

SIG_BLOCK = 4 + 64


def load_private(path):
    with open(path, "rb") as f:
        key = serialization.load_pem_private_key(f.read(), password=None)
    if not isinstance(key, ec.EllipticCurvePrivateKey) or key.curve.name != "secp256r1":
        sys.exit(f"{path}: not an ECDSA P-256 (prime256v1) private key")
    return key


def genkey(out):
    if os.path.exists(out):
        sys.exit(f"{out} already exists, not overwriting")
    pem = ec.generate_private_key(ec.SECP256R1()).private_bytes(
        serialization.Encoding.PEM, serialization.PrivateFormat.TraditionalOpenSSL, serialization.NoEncryption())
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    fd = os.open(out, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "wb") as f:
        f.write(pem)
    print(f"wrote {out} (keep it private and back it up)")


def pubkey(key_path, out):
    n = load_private(key_path).public_key().public_numbers()
    with open(out, "wb") as f:
        f.write(n.x.to_bytes(32, "big") + n.y.to_bytes(32, "big"))


def sign(key_path, inp, out):
    with open(inp, "rb") as f:
        image = f.read()
    der = load_private(key_path).sign(image, ec.ECDSA(hashes.SHA256()))
    r, s = utils.decode_dss_signature(der)
    with open(out, "wb") as f:
        f.write(image + struct.pack("<I", 0) + r.to_bytes(32, "big") + s.to_bytes(32, "big"))


def verify(pub_path, signed_path):
    with open(pub_path, "rb") as f:
        pub = f.read()
    with open(signed_path, "rb") as f:
        data = f.read()
    image, block = data[:-SIG_BLOCK], data[-SIG_BLOCK:]
    key = ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256R1(), b"\x04" + pub)
    if struct.unpack("<I", block[:4])[0] != 0:
        sys.exit("bad signature block")
    sig = utils.encode_dss_signature(int.from_bytes(block[4:36], "big"), int.from_bytes(block[36:], "big"))
    try:
        key.verify(sig, image, ec.ECDSA(hashes.SHA256()))
    except InvalidSignature:
        sys.exit("signature INVALID")
    print("signature OK")


COMMANDS = {"genkey": (genkey, 1), "pubkey": (pubkey, 2), "sign": (sign, 3), "verify": (verify, 2)}

if __name__ == "__main__":
    if len(sys.argv) < 2 or sys.argv[1] not in COMMANDS or len(sys.argv) - 2 != COMMANDS[sys.argv[1]][1]:
        sys.exit(__doc__)
    fn, _ = COMMANDS[sys.argv[1]]
    fn(*sys.argv[2:])
