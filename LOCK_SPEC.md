# HeloVT lock: BLE spec for the Helo app and server

Firmware 0.5.0. This is the contract between the lock firmware ([xiao_led_ble.ino](xiao_led_ble/xiao_led_ble.ino)),
the Helo app and the `lock-sign` server function. Production build, flashing and hardware notes are in
[PRODUCTION.md](PRODUCTION.md).

## How an unlock works

The phone never has the key. It only relays between the lock and the server.

1. Scan for the lock by service UUID and match its name (`Helo-XXXX`). Connect.
2. Subscribe to **State** notifications.
3. Read **Nonce** (16 bytes).
4. Send `{lock name, nonce, cmd}` to `lock-sign`. The server checks the rental, then returns
   `MAC = HMAC-SHA256(key, nonce || cmd)` (32 bytes) using that lock's key from Vault.
5. Write `[cmd][MAC]` (33 bytes) to **Command**, with response.
6. Wait for the State notification. Its result byte says what happened (see [Result codes](#result-codes)).
7. Disconnect when done.

`cmd`: `0x01` = unlock, `0x00` = lock now.

### Timing rules the app must follow

| Rule | Value | What it means for the app |
|---|---|---|
| Idle disconnect | 20 s | The lock drops the connection 20 s after connecting, or 20 s after the last command that returned OK. Steps 3 to 5 must finish inside that window. |
| Nonce lifetime | one attempt | A nonce is replaced after every command (good or bad) and on every connect and disconnect. Read a fresh one for each command. A signature is therefore useless after at most 20 s. |
| Bad signature | disconnect | After result 2 the lock drops the connection about 100 ms after notifying. The notification may not arrive, so treat "wrote a command, then got disconnected" as a possible bad signature. There is no lockout. |
| Unlock duration | up to 5 s | The solenoid stays powered until the latch opens (latch sensor goes from `1` to `0`) or 5 s pass, whichever is first. Then the lock relocks itself and notifies State (lock = 0). Since 0.5.0 an unlock usually ends within a fraction of a second of the rider opening the latch, so don't treat lock = 0 right after an unlock as a failure. |
| Solenoid cooldown | result 5 | Too many unlocks in a row. Wait and retry with a new nonce. With the current settings it is never more than 15 s before the next unlock is allowed. A lock command (`0x00`) is never refused for this. |
| One connection | - | The lock accepts one connection at a time and stops advertising while connected. If it can't be found, someone else may be connected. Retry for up to about 20 s. |

## Advertising

| Packet | Contents | Size |
|---|---|---|
| Advertising packet | Flags (3 bytes) + complete list of 128-bit service UUIDs: the lock service (18 bytes) | 21 of 31 bytes |
| Scan response | Complete local name `Helo-XXXX` (2 + 9 bytes) | 11 of 31 bytes |

The name does not fit in the advertising packet next to a 128-bit UUID (it would need 32 bytes), so
it is in the scan response. Phones get the scan response when they scan actively:

- **iOS** (foreground): CoreBluetooth scans actively and merges the scan response into
  `advertisementData`. Read the name from `advertisementData[CBAdvertisementDataLocalNameKey]`. Don't
  use `peripheral.name`: that is the cached GATT name, which can be stale. The lock sets it to the
  same value, but iOS may keep an old one. A background scan does not get the scan response.
- **Android**: the default scan is active. Read `ScanResult.getScanRecord().getDeviceName()`.
- **Web Bluetooth** (Chrome/Edge): filter with `{ namePrefix: "Helo-" }`.

Advertising interval: 546.25 ms (152.5 ms before 0.5.0, changed to save battery). A phone in the
foreground usually finds the lock within about 1 to 1.5 s.

**Not yet tested on phones.** The byte budget above is computed from ArduinoBLE 2.1.0, which puts
the name in the scan response. To check: in nRF Connect on an iPhone and on an Android phone, scan
with a filter on service UUID `19b10100-e8f2-537e-4f6c-d104768a1214`, and confirm the lock is listed
as `Helo-XXXX`.

## GATT

### Lock service `19b10100-e8f2-537e-4f6c-d104768a1214`

| Characteristic | UUID | Properties | Format |
|---|---|---|---|
| State | `19b10101-e8f2-537e-4f6c-d104768a1214` | read, notify | 4 bytes: `[lock][result][latch][tamper]`, see below |
| Uptime | `19b10102-e8f2-537e-4f6c-d104768a1214` | read, notify | uint32 little-endian, ms since boot. Notified once a second while connected. |
| Log | `19b10103-e8f2-537e-4f6c-d104768a1214` | read | Oldest first, 5-byte entries `[event type][uint32 ms since boot, LE]`. Last 100 events, in RAM, cleared on reboot. |
| Nonce | `19b10104-e8f2-537e-4f6c-d104768a1214` | read | 16 bytes |
| Command | `19b10105-e8f2-537e-4f6c-d104768a1214` | write (with response) | 33 bytes: `[cmd][HMAC-SHA256(key, nonce \|\| cmd)]` |

State bytes:

| Byte | Meaning |
|---|---|
| 0 lock | `0` = locked (solenoid off), `1` = unlocked (solenoid powered) |
| 1 result | Result of the last command, see [Result codes](#result-codes) |
| 2 latch | `0` = latch open, `1` = latch closed (physically secured), `0xFF` = no sensor fitted |
| 3 tamper | `0` = enclosure not opened since boot, `1` = opened since boot (stays set until reboot), `0xFF` = no sensor fitted |

State was 2 bytes before firmware 0.4.0. Read at least 2 bytes, and ignore any bytes after the ones
you know about.

"Locked" (byte 0) only means the solenoid is off. Only latch = `1` confirms that the vehicle is
physically secured. Apps should show "locked" only when byte 0 = `0` and latch is `1` (or `0xFF`, no
sensor), and "not secured" when latch is `0`. The prototype has a latch button on D1. It has no tamper
sensor, so tamper reports `0xFF`.

### Battery Service `0x180F` (standard)

| Characteristic | UUID | Properties | Format |
|---|---|---|---|
| Battery Level | `0x2A19` | read, notify | uint8, 0 to 100 % |

The lock measures it every 60 s, and not while the solenoid is on. It is a rough estimate. See
PRODUCTION.md, "Battery" for what it measures and its limits.

### Device Information Service `0x180A` (standard)

| Characteristic | UUID | Properties | Value |
|---|---|---|---|
| Model Number String | `0x2A24` | read | `HeloVT Lock` |
| Firmware Revision String | `0x2A26` | read | e.g. `0.5.0` |

Firmware revision values:

- `0.5.0`: production build, flashed correctly.
- `0.5.0-dev`: development build. Should never be on a rider's vehicle.
- `0.5.0-bootloader`: production build on a board that still has the USB bootloader. Anyone with
  a USB cable can read its key or flash other firmware. **The server should flag this lock and not
  rent it out.**

## Result codes

| Code | Name | Meaning |
|---|---|---|
| 0 | none | No command yet since boot |
| 1 | OK | Done |
| 2 | bad auth | Signature didn't match. The lock disconnects. |
| 3 | (reserved) | Was "locked out" in older firmware. No longer sent. |
| 4 | bad format | Wrong length, unknown command, or the lock couldn't make a nonce |
| 5 | cooldown | Unlock refused to protect the solenoid from overheating. Retry later. |

## Log event types

| Type | Event |
|---|---|
| 0 | Boot |
| 1 | Connected |
| 2 | Disconnected |
| 3 | Unlocked |
| 4 | Locked (by command or auto-relock) |
| 5 | Bad signature |
| 6 | Enclosure opened (tamper) |
| 7 | Latch opened |
| 8 | Latch closed |

## Server signing

```
MAC = HMAC-SHA256(key, nonce || cmd)
  key:   32 bytes, this lock's key
  nonce: the 16 bytes the app read from the lock
  cmd:   1 byte, 0x00 or 0x01
```

The server should reject anything that is not a 16-byte nonce and a `cmd` of 0 or 1.

### Test vectors

These are checked against the firmware's own HMAC code ([test/hmac_test.cpp](test/hmac_test.cpp)
compiles `hmac_sha256.h` for a PC) and against Python's standard library
([test/hmac_vectors.py](test/hmac_vectors.py)). Both pass. The server's tests should produce exactly
these MACs.

| # | key (hex) | nonce (hex) | cmd | expected MAC (hex) |
|---|---|---|---|---|
| 1 | `000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f` | `000102030405060708090a0b0c0d0e0f` | 1 | `909e5d1acd7acc32a42a318504a47a1d021530897bab0b78bce5f5481aa76269` |
| 2 | `000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f` | `ffffffffffffffffffffffffffffffff` | 0 | `8fcdc8de62ea500d7ff29cfb4e93c3bc2ce54b20cc291b5c95951a9846466069` |
| 3 | `58018b132d78672db229306d58f57628d24310274c2be7ece6ac68cd39303ab6` | `4e5a57428f3e6809d241a631f55bd2db` | 1 | `b061b472912c56054d5ffef8f73b6e0f62cf538b90657d013891f524b42487c3` |

## Key and name files

`python make_key.py --force` makes a new key and name for one lock. Run it once per lock.

`xiao_led_ble/secret.h`, compiled into the firmware:

```c
// Generated by make_key.py. Keep secret, do not commit.
#pragma once
#include <stdint.h>

#define LOCK_NAME "Helo-7F3A"

const uint8_t LOCK_KEY[32] = {
    0x.., 0x.., ... 16 bytes ...,
    0x.., 0x.., ... 16 bytes ...
};
```

`lock_key.txt` holds the same values. Line 1 is the key as 64 lowercase hex characters, and line 2 is
the name. Import the key into Vault under that lock's name.

The name is `Helo-` plus 4 random uppercase hex characters. With many locks, two could get the same
name, so the server should reject a duplicate when a lock is registered (re-run `make_key.py --force`
for that lock).

## Security notes for the server team

- The lock trusts only the HMAC. All rental rules (who, when, how long) are enforced by the server
  when it decides whether to sign.
- The BLE link is not encrypted. Anyone nearby can read State, Uptime, Log, battery and firmware
  version. The Log shows when the lock was unlocked, which is a mild privacy leak.
- A fake device can advertise the same name. It can't learn the key or unlock anything, but it can
  show the app a false State. See PRODUCTION.md, item 15.
