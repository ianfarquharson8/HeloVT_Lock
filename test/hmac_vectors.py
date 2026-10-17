"""HMAC-SHA256 test vectors for the lock protocol: MAC = HMAC-SHA256(key, nonce || cmd).

These are the same vectors as in LOCK_SPEC.md and test/hmac_test.cpp (which runs the firmware's own
hmac_sha256.h). A server implementation must produce exactly these MACs.

Run:  python test/hmac_vectors.py
"""

import hashlib
import hmac

# (key hex, nonce hex, cmd, expected MAC hex)
VECTORS = [
    ("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f",
     "000102030405060708090a0b0c0d0e0f", 1,
     "909e5d1acd7acc32a42a318504a47a1d021530897bab0b78bce5f5481aa76269"),
    ("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f",
     "ffffffffffffffffffffffffffffffff", 0,
     "8fcdc8de62ea500d7ff29cfb4e93c3bc2ce54b20cc291b5c95951a9846466069"),
    ("58018b132d78672db229306d58f57628d24310274c2be7ece6ac68cd39303ab6",
     "4e5a57428f3e6809d241a631f55bd2db", 1,
     "b061b472912c56054d5ffef8f73b6e0f62cf538b90657d013891f524b42487c3"),
]


def sign(key: bytes, nonce: bytes, cmd: int) -> bytes:
    return hmac.new(key, nonce + bytes([cmd]), hashlib.sha256).digest()


if __name__ == "__main__":
    ok = True
    for key, nonce, cmd, expected in VECTORS:
        mac = sign(bytes.fromhex(key), bytes.fromhex(nonce), cmd).hex()
        print("PASS" if mac == expected else f"FAIL (got {mac})", f"cmd={cmd} nonce={nonce}")
        ok &= mac == expected
    raise SystemExit(0 if ok else 1)
