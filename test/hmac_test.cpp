// Runs the firmware's HMAC code (xiao_led_ble/hmac_sha256.h) on a PC against the test vectors in
// test/hmac_vectors.py and LOCK_SPEC.md, to prove the firmware matches standard HMAC-SHA256.
//
// Build and run with any C++ compiler, e.g.:
//   g++ test/hmac_test.cpp -o hmac_test && ./hmac_test
//   python -m ziglang c++ test/hmac_test.cpp -o hmac_test.exe && hmac_test.exe

#include <stdio.h>
#include "../xiao_led_ble/hmac_sha256.h"

struct Vector { const char* key; const char* nonce; uint8_t cmd; const char* mac; };

static const Vector VECTORS[] = {
  {"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f",
   "000102030405060708090a0b0c0d0e0f", 1,
   "909e5d1acd7acc32a42a318504a47a1d021530897bab0b78bce5f5481aa76269"},
  {"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f",
   "ffffffffffffffffffffffffffffffff", 0,
   "8fcdc8de62ea500d7ff29cfb4e93c3bc2ce54b20cc291b5c95951a9846466069"},
  {"58018b132d78672db229306d58f57628d24310274c2be7ece6ac68cd39303ab6",
   "4e5a57428f3e6809d241a631f55bd2db", 1,
   "b061b472912c56054d5ffef8f73b6e0f62cf538b90657d013891f524b42487c3"},
};

static void fromHex(const char* hex, uint8_t* out, size_t len) {
  for (size_t i = 0; i < len; i++) sscanf(hex + 2 * i, "%2hhx", &out[i]);
}

int main() {
  int failures = 0;
  for (const Vector& v : VECTORS) {
    // Build the message exactly as signatureValid() in the firmware does: nonce || cmd
    uint8_t key[32], msg[17], expected[32], mac[32];
    fromHex(v.key, key, 32);
    fromHex(v.nonce, msg, 16);
    msg[16] = v.cmd;
    fromHex(v.mac, expected, 32);

    hmac_sha256::hmac(key, sizeof(key), msg, sizeof(msg), mac);
    bool ok = memcmp(mac, expected, 32) == 0;
    printf("%s cmd=%d nonce=%s\n", ok ? "PASS" : "FAIL", v.cmd, v.nonce);
    if (!ok) failures++;
  }
  return failures;
}
