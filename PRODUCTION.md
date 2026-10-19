# HeloVT lock: production build, flashing and hardware

Firmware 0.5.0. The app-facing protocol is in [LOCK_SPEC.md](LOCK_SPEC.md).

## Review status

| # | Item | Status |
|---|---|---|
| 1 | Remove the global lockout | **Done.** A bad signature drops only that connection. |
| 2 | Production build flag | **Done, flashing not yet tested on a board.** `PRODUCTION` turns on readback protection, turns off Serial output and turns on the watchdog. **The USB bootloader must also be removed** (see [below](#why-the-usb-bootloader-has-to-go)). |
| 3 | Fail-secure | **Firmware side done**, and it depends on two hardware requirements. See [Fail-secure](#fail-secure). |
| 4 | Watchdog, reboot if BLE fails to start | **Done** (8 s, production builds). |
| 5 | Battery Service | **Done.** It only measures a LiPo on the XIAO's BAT pads. See [Battery](#battery). |
| 6 | Firmware version | **Done.** Device Information Service, Firmware Revision String. |
| 7 | Name reaches phones | **Byte budget checked, not yet tested on phones.** See LOCK_SPEC.md, "Advertising". |
| 8 | HMAC test vectors | **Done**, and checked against the firmware's own code. See LOCK_SPEC.md. |
| 9 | Latch sensor | **On in the prototype:** a tactile button on D1 (`LATCH_PIN`). Production needs a sealed switch (see [Hardware](#hardware-requirements-open-to-be-confirmed)). |
| 10 | Tamper sensor | **Firmware ready, needs hardware.** Set `TAMPER_PIN`. |
| 11 | Advertising interval, battery life | **Software power work done in 0.5.0 and compiled, not yet tested on a board. Battery life not measured.** See [Power](#power-and-battery-life). |
| 12 | Solenoid duty cycle | **Done:** a heat budget, with limits to be set from the solenoid's datasheet. See [Solenoid duty cycle](#solenoid-duty-cycle). |
| 13 to 15 | Later items | Notes only. See [Later](#later-not-for-the-beta). |
| - | Hardware | **Open.** Requirements are below. None of it is built or decided yet. |

## Fail-secure

The lock stays locked whenever the board is off, resetting or crashed:

- The solenoid is powered only while D0 is driven HIGH. The first thing `setup()` does is drive it LOW.
- On power loss the solenoid has no power at all.
- During a reset, watchdog reboot or boot, the chip's pins float for up to about a second until
  `setup()` runs. **A 100 kΩ pull-down from the MOSFET gate to ground is required** to hold it off
  during that time. Without it, a floating gate could partly switch the solenoid on.
- **The solenoid must be a power-to-unlock (fail-secure) type.** A power-to-lock (fail-safe) solenoid
  would open whenever power is lost.

What this means in practice: if the lock's battery dies, the vehicle stays locked and nobody can open
it over BLE. Plan for that, either with a mechanical override for the host or by powering the lock
from the scooter's main pack with a reserve (see [Power](#power-and-battery-life)).

## Why the USB bootloader has to go

The XIAO ships with a USB bootloader (Adafruit nRF52 Bootloader 0.6.1). Double-tapping reset with a
USB cable plugged in starts it. Then:

- **It shows a USB drive with `CURRENT.UF2`, a copy of the whole firmware including the key**
  (checked in the bootloader's source, `src/usb/uf2/ghostfat.c`).
- It accepts any new firmware over the USB drive or serial DFU, with no signature check. Someone with
  a cable could flash "always unlock" firmware.

Readback protection (APPROTECT) only blocks a debug probe. It does nothing against the bootloader,
which runs on the chip itself. So production boards need **both**: the bootloader removed and
APPROTECT on. The firmware's revision string reports `-bootloader` if a production build finds the
bootloader still installed.

Because each lock has its own key, a key taken from one board opens only that lock.

## Production flashing steps

**These steps have not been run on a board yet. Try them on one spare board first.**

You need:

- An SWD debug probe: a SEGGER J-Link, or the nRF52840 DK's onboard J-Link through its "Debug out"
  header. A J-Link EDU is for non-commercial use only.
- nRF Command Line Tools (`nrfjprog`). Nordic is replacing it with `nrfutil device`, which has the
  equivalent commands `recover`, `program` and `protection-set`.
- Nordic's S140 v7.3.0 SoftDevice, `s140_nrf52_7.3.0_softdevice.hex`, from nordicsemi.com. The
  firmware is linked at `0x27000`, after the SoftDevice. At boot the MBR (inside that hex) starts the
  SoftDevice, which hands over to the firmware. The firmware uses its own BLE stack. It only needs
  the SoftDevice to boot.

Wiring: probe SWDIO, SWCLK, GND and VTref (3V3) to the XIAO's SWD pads on the underside of the board.

For each lock:

```sh
# 1. New key and name for this lock. Put the key into Vault under the name, then delete
#    lock_key.txt from the build machine once it's safely stored.
python make_key.py --force

# 2. Production build -> build/xiao_led_ble.ino.hex
arduino-cli compile --fqbn Seeeduino:mbed:xiaonRF52840 \
  --build-property build.extra_flags=-DPRODUCTION --output-dir build xiao_led_ble

# 3. Erase everything, including the USB bootloader and its settings (UICR)
nrfjprog --recover

# 4. SoftDevice (boot chain only), then the firmware
nrfjprog --program s140_nrf52_7.3.0_softdevice.hex --verify
nrfjprog --program build/xiao_led_ble.ino.hex --verify

# 5. Readback protection. The firmware also turns it on at first boot if this step is missed.
nrfjprog --rbp ALL
nrfjprog --pinreset
```

Check each board:

1. `nrfjprog --readcode check.hex` fails with a readback-protection error.
2. With USB plugged in, double-tap reset. No USB drive appears.
3. In nRF Connect: the board advertises as `Helo-XXXX`, and Device Information shows firmware
   `0.5.0` (not `-dev` or `-bootloader`).
4. A server-signed unlock works, and D0 measures about 3.3 V while unlocked.

If the board doesn't advertise after step 4, it means the SoftDevice-to-firmware handover assumed
above doesn't work. Report it rather than shipping that board with the bootloader on it.

To reuse a production board for development: `nrfjprog --recover` (this erases the key too), then
flash the Seeed bootloader hex from the Arduino core's `bootloaders/` folder.

### What `PRODUCTION` changes

- **APPROTECT**: on first boot the firmware writes `UICR.APPROTECT = Enabled` and reboots. After
  that, a debugger can't read flash. Only a full erase undoes it.
- **USB off.** The Arduino core always starts USB before `setup()`, and the firmware shuts it down
  again (`PluggableUSBD().deinit()`). The board doesn't show up as a USB device and prints nothing.
  This saves power when the board is fed through its 5 V pin. **Not yet tested on a board.** If a
  production build doesn't advertise, try removing that line first.
- **Watchdog** (8 s): if the main loop stops, the chip reboots and comes back up locked. It is off in
  dev builds because the USB bootloader doesn't feed it and an upload would get interrupted.
  `#define USE_WATCHDOG` turns it on for testing (double-tap reset before each upload).
- If `BLE.begin()` fails, the board reboots (all builds) instead of hanging.

**Physical protection still matters.** Mount the board inside the enclosure with no external USB
access, and pot or coat it so the SWD pads and USB port can't be reached without destroying it.

## Battery

How it's measured: the XIAO has a 1 MΩ / 510 kΩ voltage divider from its BAT pads to pin P0.31. The
firmware reads it with the ADC (2.4 V internal reference, 12-bit, averaged over 8 samples) and
multiplies by 1510 / 510. It turns that into a percentage with a rough single-cell LiPo curve
(3.3 V = 0 %, 3.7 V = 30 %, 3.8 V = 50 %, 4.2 V = 100 %). It measures every 60 s and skips any reading
while the solenoid is on, because the solenoid's current pulls the voltage down.

Limits:

- **This only measures a single LiPo cell connected to the XIAO's BAT pads.** If the lock is powered
  from the scooter's pack through a 5 V regulator, this reading is meaningless. A divider from the
  pack to a free analog pin (A1 to A5) would be needed, and the firmware changed to match.
- On USB power with no battery, it reads the charger's output, about 100 %.
- The divider ratio is the XIAO's published value. Check the reading against a multimeter on a few
  boards.

## Power and battery life

What the firmware does to save power (0.5.0, compiled but **not yet measured or tested on a board**):

| Change | Why | Constant / function |
|---|---|---|
| Sensor pull-ups on only during a read | The internal ~13 kΩ pull-up draws about 250 µA through a closed switch, and the latch is closed whenever the vehicle is parked locked. | `switchClosed()` |
| Slow loop while idle | Wakes every 500 ms while locked and not connected, and every 50 ms while unlocked or connected. A connecting phone wakes it at once. Latch and tamper changes made while nobody is connected can be logged up to about 1 s late. | `IDLE_POLL_MS`, `POLL_MS` |
| Advertising interval 546.25 ms (was 152.5 ms) | About 3.5 times less advertising energy. Phones still find the lock in about 1 to 1.5 s. 152.5, 417.5, 760 and 1022.5 ms are other Apple-recommended values. | `ADVERTISING_INTERVAL` |
| Unlock ends when the latch opens | The solenoid used to stay on for the full 5 s. Now it turns off as soon as the latch sensor reads open, which cuts most of each unlock's energy and heat. Turn it off if the mechanism needs the solenoid held while the latch is closed again. | `RELEASE_ON_LATCH_OPEN` |
| DC/DC regulator on | About 30 to 40 % less current while the CPU or radio runs. The XIAO has the inductor (Zephyr's board file for it uses DC/DC mode). | `NRF_POWER->DCDCEN` in `setup()` |
| Onboard flash in deep power-down | The unused 2 MB QSPI flash otherwise sits in standby. | `flashDeepPowerDown()` |
| USB off (production) | See [What `PRODUCTION` changes](#what-production-changes). | `setup()` |

Not changed:

- **Uptime notifications** (once a second while connected) stay, because the apps use them.
  Connections last 20 s at most.
- **Battery divider**: about 3 µA through the 1.5 MΩ divider all the time, because P0.14 must stay
  LOW to protect P0.31.

Next steps:

- **Measure** a board with a Nordic Power Profiler Kit II on the BAT pads (not USB), dev and
  production builds, idle and during an unlock. Record the numbers here.
- **The Arduino mbed core sets the floor.** Even after these changes, expect the idle draw to be
  set by the core rather than the firmware: likely hundreds of µA, not the 10 to 30 µA an nRF52840
  can reach. Moving to nRF Connect SDK (item 13) is the main remaining battery-life fix.
- **Pull-in then hold**: once the solenoid is chosen, run it at full power for about 100 to 200 ms,
  then PWM it at the lower holding current from its datasheet. It matters less now that
  `RELEASE_ON_LATCH_OPEN` ends most unlocks early, but it still helps when nobody opens the latch.
  Note that PWM hardware keeps running if the firmware hangs, until the watchdog reboots the board.
- **The solenoid dominates.** A typical 12 V lock solenoid draws 0.5 to 1 A, so one 5 s unlock uses
  about 0.7 to 1.4 mAh at 12 V. A single LiPo cell can't drive it without a boost converter.
- **Recommendation**: power the lock from the scooter's main pack. Tap the pack before the scooter's
  power switch and use a wide-input buck converter with low standby current. Keep a cut-off reserve
  so the lock can still open when the scooter's battery is "empty".

## Solenoid duty cycle

The firmware keeps a heat budget. Time with the solenoid on builds heat. Time off drains it 3 times
slower, which works out to about 25 % duty cycle sustained. An unlock is refused with result 5
(cooldown) if a full 5 s unlock would go past 10 s of continuous on-time from cold. In practice: two
unlocks back to back are allowed, then the next one has to wait up to 15 s.

The constants are `HEAT_LIMIT_MS` and `COOL_RATIO` in the firmware. **Set them from the chosen
solenoid's datasheet** (its rated maximum on-time and duty cycle). The budget resets on reboot.

## Hardware requirements (open, to be confirmed)

None of this is built or decided yet.

- **Solenoid driver**: D0 → 100 Ω → gate of a logic-level N-channel MOSFET whose on-resistance is
  specified at 2.5 V gate drive (e.g. AO3400A for a solenoid of up to about 2 A). Add a **100 kΩ
  pull-down from gate to ground** (required for fail-secure). Connect the solenoid between +V and the
  drain, with a **flyback diode** across it (e.g. SS34 / 1N5819, cathode to +V). The solenoid supply
  and the XIAO share a ground.
- **Solenoid**: power-to-unlock (fail-secure) type, with a rated on-time of at least 5 s.
- **Power source**: undecided. See [Power](#power-and-battery-life).
- **Enclosure and weatherproofing**: undecided. Aim for IP67 if it's mounted outside, and pot or
  conformal-coat the electronics.
- **Mounting**: undecided. Tamper-resistant fasteners, reachable only when the lock is open, ideally
  inside the frame.
- **Cut and pry resistance**: undecided. This depends on the shackle or cable and the housing, not
  the electronics.
- **Sensors**: for the latch (item 9), a reed or limit switch that closes when the latch is engaged.
  For tamper (item 10), a switch that is closed while the lid is shut. Each is wired from its pin to
  GND, so a cut wire reads as "open".

## Later (not for the beta)

**13. Signed OTA updates.** Neither the Arduino mbed core nor the Adafruit bootloader can verify
signed firmware. The practical route is moving the firmware to Nordic's nRF Connect SDK (Zephyr +
MCUboot with signed images). It's a port, but the protocol stays the same.

**14. OTA key rotation.** Add a command that carries a new key, encrypted under the current key and
signed with it (same nonce scheme). The firmware would store the key in flash instead of compiling it
in. APPROTECT still protects it there.

**15. Relay and spoofing.** A fake device advertising a lock's name can't learn the key or unlock
anything, because signatures only work for the real lock's nonce. Two risks remain:

- **False status**: a fake lock can tell the app "locked" when it isn't. Fix: the lock signs its
  State together with the nonce, and the server checks it.
- **Relay**: someone relays between a renter's phone and the real lock, to unlock it while the renter
  is elsewhere. This only works during that renter's own active rental, and within the 20 s nonce
  window. Fixes: the server checks the phone's location against the vehicle's, or the lock measures
  round-trip timing.
