// HeloVT lock - XIAO nRF52840 firmware: BLE-controlled lock (solenoid on D0, latch button on D1)
// Board package: "Seeed nRF52 mbed-enabled Boards" -> "Seeed XIAO BLE - nRF52840"
// Library: ArduinoBLE
// Before uploading, run `python make_key.py` in the project folder to create secret.h.
// The app-facing protocol (UUIDs, data formats, result codes) is in LOCK_SPEC.md. The production
// build, flashing steps and hardware requirements are in PRODUCTION.md.
//
// SECURITY: commands must be signed with a secret key shared only by the board and Helo's server
// (HMAC-SHA256 challenge-response). The key never goes over the air and is never on a phone.
//   1. App reads the Nonce characteristic: 16 random bytes, valid for one command.
//   2. The server signs it, and the app writes the Command characteristic, 33 bytes:
//        [cmd][HMAC-SHA256(key, nonce || cmd)]    cmd: 0x01 = unlock, 0x00 = lock now
//   3. Board checks the HMAC, acts on it, picks a new nonce, then notifies State.
// The nonce changes after every attempt and on every connect and disconnect, so a recorded command
// can't be replayed. A bad signature drops that connection. There is no lockout: the 256-bit key
// can't be guessed anyway, and a lockout would let a stranger block a renter.
// The board takes one connection at a time, so it drops any connection that goes IDLE_MS without
// a valid command. Someone without the key can't hold the lock and block the real user.
// An unlock relocks by itself after UNLOCK_MS.
//
// FAIL-SECURE: the solenoid is powered only while D0 is driven HIGH. On power loss, reset, watchdog
// reboot or crash, D0 is LOW or floating (held LOW by the MOSFET gate pull-down, see PRODUCTION.md),
// so the solenoid is unpowered and a power-to-unlock solenoid stays locked.
//
// Each lock advertises its own name from secret.h (e.g. "Helo-7F3A"), so apps can tell locks apart.
//
// Characteristics (service SERVICE_UUID):
//   State   (read/notify): [lock: 0 = locked, 1 = unlocked][result of last command: CmdResult]
//                          [latch: 0 = open, 1 = closed][tamper: 0 = ok, 1 = opened since boot]
//                          (latch and tamper are SENSOR_NOT_FITTED when there is no sensor)
//   Uptime  (read/notify): uint32 ms since boot, notified once per second while connected.
//   Log     (read): oldest-first list of 5-byte entries [type: EventType][uint32 ms, little-endian]
//   Nonce   (read): 16 bytes
//   Command (write): 33 bytes, as above
// Plus the standard Battery Service (0x180F) and Device Information Service (0x180A).
// The log is kept in RAM and is lost on power-off/reset.
//
// The BLE link itself is not encrypted, so anyone nearby can read the state, log, battery level
// and firmware version. They can't unlock without the key.
//
// POWER (see PRODUCTION.md, "Power and battery life"): the CPU sleeps in BLE.poll() between events,
// waking every POLL_MS while unlocked or connected and every IDLE_POLL_MS otherwise. The sensor
// pull-ups are on only during each read, the DC/DC regulator is on, the unused onboard flash is put
// into deep power-down, and production builds shut USB off. An unlock ends as soon as the latch opens.

// ---- Build options ----
// PRODUCTION: readback protection, no Serial output, watchdog. Build it with
//   arduino-cli compile --build-property build.extra_flags=-DPRODUCTION ...
// and flash it only as PRODUCTION.md describes (it also needs the USB bootloader removed).
// #define PRODUCTION

// The watchdog is on in PRODUCTION builds. The USB bootloader doesn't feed it and it keeps running
// through the reset into the bootloader, so on a dev board it would reboot the board mid-upload.
// Define USE_WATCHDOG to test it on a dev board anyway (then double-tap reset before each upload).
// #define USE_WATCHDOG
#if defined(PRODUCTION) && !defined(USE_WATCHDOG)
#define USE_WATCHDOG
#endif

#define FIRMWARE_VERSION "0.5.0"

// Optional sensors: a switch from the pin to GND (the pin's internal pull-up is switched on for each
// read), so a cut wire reads as open. Uncomment the ones that are fitted.
//   LATCH_PIN:  switch closed = latch/shackle physically engaged
//   TAMPER_PIN: switch closed = enclosure lid closed
#define LATCH_PIN D1  // prototype: tactile button, pressed when the lock is closed
// #define TAMPER_PIN D2

// Cut solenoid power as soon as the latch opens, instead of holding it for the full UNLOCK_MS.
// Saves most of the energy of each unlock. Turn off if the mechanism needs the solenoid held while
// the latch is being closed again.
#define RELEASE_ON_LATCH_OPEN

#include <ArduinoBLE.h>
#include <utility/HCI.h>
#ifdef PRODUCTION
#include <USB/PluggableUSBDevice.h>
#endif
#include "hmac_sha256.h"

#if __has_include("secret.h")
#include "secret.h"  // defines LOCK_KEY and LOCK_NAME
#else
#error "secret.h is missing. Run `python make_key.py` in the project folder to create it."
#endif
#ifndef LOCK_NAME
#error "secret.h has no LOCK_NAME. Run `python make_key.py` again to add one (it keeps the key)."
#endif

#ifdef PRODUCTION
#define LOG(x) do {} while (0)
#define LOGLN(x) do {} while (0)
#else
#define LOG(x) Serial.print(x)
#define LOGLN(x) Serial.println(x)
#endif

// These must match LOCK_SPEC.md, laptop_app.py and web/index.html
#define SERVICE_UUID "19b10100-e8f2-537e-4f6c-d104768a1214"
#define STATE_CHAR_UUID "19b10101-e8f2-537e-4f6c-d104768a1214"
#define UPTIME_CHAR_UUID "19b10102-e8f2-537e-4f6c-d104768a1214"
#define LOG_CHAR_UUID "19b10103-e8f2-537e-4f6c-d104768a1214"
#define NONCE_CHAR_UUID "19b10104-e8f2-537e-4f6c-d104768a1214"
#define COMMAND_CHAR_UUID "19b10105-e8f2-537e-4f6c-d104768a1214"

const int NONCE_LEN = 16;
const int MAC_LEN = 32;
const int COMMAND_LEN = 1 + MAC_LEN;

const unsigned long UNLOCK_MS = 5000;    // how long an unlock lasts before relocking
const unsigned long IDLE_MS = 20000;     // drop a connection after this long without a valid command
const unsigned long WATCHDOG_MS = 8000;  // reboot if the loop stops running for this long
const unsigned long POLL_MS = 50;        // longest the loop sleeps waiting for BLE events, while
                                         // unlocked or connected
const unsigned long IDLE_POLL_MS = 500;  // the same while locked and not connected (sensor reads
                                         // and their timestamps can lag by up to 2 x this)
const uint16_t ADVERTISING_INTERVAL = 874;  // x 0.625 ms = 546.25 ms, one of Apple's recommended values

// Solenoid heat budget. On-time builds heat, which drains COOL_RATIO times slower while it's off
// (so about 1 / (1 + COOL_RATIO) = 25% duty cycle sustained). An unlock is refused if a full
// UNLOCK_MS wouldn't fit under HEAT_LIMIT_MS. Set both from the solenoid's datasheet.
const unsigned long HEAT_LIMIT_MS = 10000;  // most continuous on-time allowed from cold
const unsigned long COOL_RATIO = 3;

enum Command : uint8_t { CMD_LOCK = 0, CMD_UNLOCK = 1 };
// 3 was "locked out" in older firmware and is no longer sent
enum CmdResult : uint8_t {
  RES_NONE = 0, RES_OK = 1, RES_BAD_AUTH = 2, RES_BAD_FORMAT = 4, RES_COOLDOWN = 5
};
const uint8_t SENSOR_NOT_FITTED = 0xFF;

// Keep the most recent MAX_EVENTS events (5 bytes each, must fit in 512 bytes)
const int MAX_EVENTS = 100;
const int EVENT_SIZE = 5;

enum EventType : uint8_t {
  EVT_BOOT = 0, EVT_CONNECT = 1, EVT_DISCONNECT = 2, EVT_UNLOCK = 3, EVT_LOCK = 4, EVT_AUTH_FAIL = 5,
  EVT_TAMPER = 6, EVT_LATCH_OPEN = 7, EVT_LATCH_CLOSED = 8
};

BLEService lockService(SERVICE_UUID);
BLECharacteristic stateChar(STATE_CHAR_UUID, BLERead | BLENotify, 4);
BLEUnsignedLongCharacteristic uptimeChar(UPTIME_CHAR_UUID, BLERead | BLENotify);
BLECharacteristic logChar(LOG_CHAR_UUID, BLERead, MAX_EVENTS * EVENT_SIZE);
BLECharacteristic nonceChar(NONCE_CHAR_UUID, BLERead, NONCE_LEN);
BLECharacteristic commandChar(COMMAND_CHAR_UUID, BLEWrite, COMMAND_LEN);

BLEService batteryService("180F");
BLEUnsignedCharCharacteristic batteryLevelChar("2A19", BLERead | BLENotify);  // percent

BLEService deviceInfoService("180A");
BLEStringCharacteristic modelChar("2A24", BLERead, 20);
BLEStringCharacteristic firmwareRevChar("2A26", BLERead, 20);

// The XIAO's onboard LEDs are active LOW (LOW = on). Kept off.
const int LED_PIN = LED_BUILTIN;

// Solenoid control output (active HIGH: HIGH = 3.3V). D0 can only source a few mA, so it must
// drive the solenoid through a MOSFET with a flyback diode (see PRODUCTION.md).
const int SOLENOID_PIN = D0;

bool unlocked = false;
unsigned long unlockedAt = 0;
uint8_t lastResult = RES_NONE;

uint8_t nonce[NONCE_LEN];

#ifdef LATCH_PIN
bool latchClosed = false;
#endif
bool tampered = false;  // enclosure opened since boot

// Ring buffer of events
uint8_t eventTypes[MAX_EVENTS];
uint32_t eventTimes[MAX_EVENTS];
int eventStart = 0;
int eventCount = 0;

void logEvent(uint8_t type) {
  uint32_t now = millis();

  int idx;
  if (eventCount < MAX_EVENTS) {
    idx = (eventStart + eventCount) % MAX_EVENTS;
    eventCount++;
  } else {
    // Full: overwrite the oldest event
    idx = eventStart;
    eventStart = (eventStart + 1) % MAX_EVENTS;
  }
  eventTypes[idx] = type;
  eventTimes[idx] = now;

  // Rebuild the log characteristic, oldest first
  static uint8_t buf[MAX_EVENTS * EVENT_SIZE];
  for (int i = 0; i < eventCount; i++) {
    int j = (eventStart + i) % MAX_EVENTS;
    uint8_t* p = buf + i * EVENT_SIZE;
    p[0] = eventTypes[j];
    p[1] = eventTimes[j] & 0xFF;
    p[2] = (eventTimes[j] >> 8) & 0xFF;
    p[3] = (eventTimes[j] >> 16) & 0xFF;
    p[4] = (eventTimes[j] >> 24) & 0xFF;
  }
  logChar.writeValue(buf, eventCount * EVENT_SIZE);

#ifndef PRODUCTION
  const char* names[] = {"BOOT", "CONNECT", "DISCONNECT", "UNLOCK", "LOCK", "AUTH FAIL",
                         "TAMPER", "LATCH OPEN", "LATCH CLOSED"};
  LOG("[");
  LOG(now);
  LOG(" ms] ");
  LOGLN(names[type]);
#endif
}

void publishState() {
  uint8_t value[4] = {unlocked ? (uint8_t)1 : (uint8_t)0, lastResult, SENSOR_NOT_FITTED, SENSOR_NOT_FITTED};
#ifdef LATCH_PIN
  value[2] = latchClosed ? 1 : 0;
#endif
#ifdef TAMPER_PIN
  value[3] = tampered ? 1 : 0;
#endif
  stateChar.writeValue(value, sizeof(value));
}

// Solenoid heat, in ms of on-time x COOL_RATIO
unsigned long heat = 0;
unsigned long heatUpdatedAt = 0;

void updateHeat() {
  unsigned long now = millis();
  unsigned long elapsed = now - heatUpdatedAt;
  heatUpdatedAt = now;
  if (unlocked) heat += elapsed * COOL_RATIO;
  else heat = heat > elapsed ? heat - elapsed : 0;
}

// True if a full unlock fits in the heat budget
bool solenoidCanUnlock() {
  updateHeat();
  return heat + UNLOCK_MS * COOL_RATIO <= HEAT_LIMIT_MS * COOL_RATIO;
}

void setLock(bool unlock) {
  updateHeat();  // count the time up to now at the old state
  unlocked = unlock;
  unlockedAt = millis();
  digitalWrite(SOLENOID_PIN, unlock ? HIGH : LOW);
}

// Fill buf (a multiple of 8 bytes) with random bytes from the BLE controller, which reads the
// nRF52840's hardware RNG. Needs BLE.begin() first. (mbed's trng_api is not used because on this
// board it needs the CryptoCell powered up, and hangs without it.)
bool fillRandom(uint8_t* buf, size_t len) {
  for (size_t i = 0; i < len; i += 8) {
    int tries = 0;
    while (HCI.leRand(buf + i) != 0) {
      if (++tries >= 10) return false;
    }
  }
  return true;
}

// False if the last nonce couldn't be generated. Commands are refused until one is.
bool nonceValid = false;

void newNonce() {
  nonceValid = fillRandom(nonce, NONCE_LEN);
  if (!nonceValid) LOGLN("Could not get random bytes for the nonce");
  nonceChar.writeValue(nonce, NONCE_LEN);
}

// True if mac == HMAC-SHA256(LOCK_KEY, nonce || cmd)
bool signatureValid(uint8_t cmd, const uint8_t* mac) {
  uint8_t msg[NONCE_LEN + 1];
  memcpy(msg, nonce, NONCE_LEN);
  msg[NONCE_LEN] = cmd;

  uint8_t expected[MAC_LEN];
  hmac_sha256::hmac(LOCK_KEY, sizeof(LOCK_KEY), msg, sizeof(msg), expected);

  // Compare every byte so the time taken doesn't reveal how much matched
  uint8_t diff = 0;
  for (int i = 0; i < MAC_LEN; i++) diff |= expected[i] ^ mac[i];
  return diff == 0;
}

void handleCommand(BLEDevice& central) {
  uint8_t msg[COMMAND_LEN];
  int len = commandChar.readValue(msg, COMMAND_LEN);

  if (!nonceValid || len != COMMAND_LEN || (msg[0] != CMD_LOCK && msg[0] != CMD_UNLOCK)) {
    lastResult = RES_BAD_FORMAT;
  } else if (!signatureValid(msg[0], msg + 1)) {
    lastResult = RES_BAD_AUTH;
  } else if (msg[0] == CMD_UNLOCK && !solenoidCanUnlock()) {
    lastResult = RES_COOLDOWN;
  } else {
    lastResult = RES_OK;
  }

  // Every attempt uses up the nonce. Set the new one before notifying, so the app
  // reads a fresh nonce for its next command.
  newNonce();

  if (lastResult == RES_OK) {
    setLock(msg[0] == CMD_UNLOCK);
    logEvent(unlocked ? EVT_UNLOCK : EVT_LOCK);
  } else if (lastResult == RES_BAD_AUTH) {
    logEvent(EVT_AUTH_FAIL);
  }
  publishState();

  if (lastResult == RES_BAD_AUTH) {
    // Drop only this connection. Give the State notification a moment to go out first.
    unsigned long start = millis();
    while (millis() - start < 100) BLE.poll(10);
    central.disconnect();
  }
}

// Auto-relock. Runs whether or not anything is connected.
void updateTimers() {
  updateHeat();
  if (unlocked && millis() - unlockedAt >= UNLOCK_MS) {
    setLock(false);
    logEvent(EVT_LOCK);
    publishState();
  }
}

// True if the switch from pin to GND is closed. The internal pull-up (about 13 kOhm) is on only for
// the read: left on, a closed switch would draw about 250 uA for as long as the lock is closed.
// Between reads the pin is a disconnected input, which draws nothing.
const unsigned int SWITCH_SETTLE_US = 50;  // time for the pull-up to charge the wire

bool switchClosed(int pin) {
  uint32_t n = digitalPinToPinName(pin);  // nRF GPIO number: 0-31 port 0, 32-47 port 1
  NRF_GPIO_Type* port = n < 32 ? NRF_P0 : NRF_P1;
  uint32_t bit = n & 31;
  port->PIN_CNF[bit] = (GPIO_PIN_CNF_DIR_Input << GPIO_PIN_CNF_DIR_Pos) |
                       (GPIO_PIN_CNF_INPUT_Connect << GPIO_PIN_CNF_INPUT_Pos) |
                       (GPIO_PIN_CNF_PULL_Pullup << GPIO_PIN_CNF_PULL_Pos);
  delayMicroseconds(SWITCH_SETTLE_US);
  bool closed = ((port->IN >> bit) & 1) == 0;
  port->PIN_CNF[bit] = (GPIO_PIN_CNF_DIR_Input << GPIO_PIN_CNF_DIR_Pos) |
                       (GPIO_PIN_CNF_INPUT_Disconnect << GPIO_PIN_CNF_INPUT_Pos) |
                       (GPIO_PIN_CNF_PULL_Disabled << GPIO_PIN_CNF_PULL_Pos);
  return closed;
}

// Read the switches at most every 50 ms (each loop pass; see POLL_MS and IDLE_POLL_MS). A change
// counts once two reads in a row agree (debounce).
void updateSensors() {
  static unsigned long lastRead = 0;
  if (millis() - lastRead < 50) return;
  lastRead = millis();
  bool changed = false;

#ifdef LATCH_PIN
  static bool lastClosed = latchClosed;
  bool closed = switchClosed(LATCH_PIN);
  if (closed == lastClosed && closed != latchClosed) {
    latchClosed = closed;
    logEvent(closed ? EVT_LATCH_CLOSED : EVT_LATCH_OPEN);
    changed = true;
#ifdef RELEASE_ON_LATCH_OPEN
    // The latch is open, so the solenoid's job is done
    if (!closed && unlocked) {
      setLock(false);
      logEvent(EVT_LOCK);
    }
#endif
  }
  lastClosed = closed;
#endif

#ifdef TAMPER_PIN
  static bool lastOpen = false;
  bool open = !switchClosed(TAMPER_PIN);
  if (open && lastOpen && !tampered) {
    tampered = true;  // stays set until reboot, so opening it while nobody is connected still shows
    logEvent(EVT_TAMPER);
    changed = true;
  }
  lastOpen = open;
#endif

  if (changed) publishState();
}

// Battery: the XIAO measures its LiPo (the BAT pads) through an onboard 1M / 510k divider into
// P0.31 (PIN_VBAT), which works while P0.14 (PIN_VBAT_ENABLE) is LOW. P0.14 is kept LOW all the time:
// if it were HIGH, P0.31 would see the full battery voltage, above the pin's 3.6 V limit.
const float VBAT_DIVIDER = (1000.0f + 510.0f) / 510.0f;
const unsigned long BATTERY_PERIOD_MS = 60000;

uint16_t readBatteryMv() {
  uint32_t sum = 0;
  for (int i = 0; i < 8; i++) sum += analogRead(PIN_VBAT);
  return sum / 8.0f * 2400.0f / 4096.0f * VBAT_DIVIDER;  // 12 bits, 2.4 V full scale
}

// Rough LiPo state of charge from its resting voltage
uint8_t batteryPercent(uint16_t mv) {
  static const uint16_t MV[] = {3300, 3500, 3600, 3700, 3800, 3900, 4000, 4100, 4200};
  static const uint8_t PCT[] = {   0,    5,   12,   30,   50,   65,   80,   90,  100};
  if (mv <= MV[0]) return 0;
  for (int i = 1; i < 9; i++) {
    if (mv < MV[i]) return PCT[i - 1] + (PCT[i] - PCT[i - 1]) * (mv - MV[i - 1]) / (MV[i] - MV[i - 1]);
  }
  return 100;
}

void updateBattery() {
  static bool measured = false;
  static unsigned long lastRead = 0;
  // Skip while the solenoid is on: its current pulls the battery voltage down
  if (unlocked || (measured && millis() - lastRead < BATTERY_PERIOD_MS)) return;
  measured = true;
  lastRead = millis();
  uint8_t percent = batteryPercent(readBatteryMv());
  if (percent != batteryLevelChar.value()) batteryLevelChar.writeValue(percent);
}

// Put the XIAO's onboard 2 MB QSPI flash (P25Q16H) into deep power-down. The firmware doesn't use
// it, and in standby it draws more than the nRF52840 does asleep. Sends the 0xB9 command by
// bit-banging SPI mode 0 on the flash pins (P0.21 SCK, P0.25 CS, P0.20 IO0, P0.22 WP, P0.23 HOLD,
// from the core's variant.cpp). CS is then held HIGH. To use the flash again, send 0xAB first.
void flashDeepPowerDown() {
  const uint32_t SCK = 1u << 21, CS = 1u << 25, IO0 = 1u << 20, WP = 1u << 22, HOLD = 1u << 23;
  NRF_P0->OUTSET = CS | WP | HOLD;
  NRF_P0->OUTCLR = SCK | IO0;
  NRF_P0->DIRSET = CS | SCK | IO0 | WP | HOLD;
  delayMicroseconds(1);
  NRF_P0->OUTCLR = CS;
  delayMicroseconds(1);
  for (int bit = 7; bit >= 0; bit--) {
    if ((0xB9 >> bit) & 1) NRF_P0->OUTSET = IO0;
    else NRF_P0->OUTCLR = IO0;
    delayMicroseconds(1);
    NRF_P0->OUTSET = SCK;
    delayMicroseconds(1);
    NRF_P0->OUTCLR = SCK;
  }
  NRF_P0->OUTCLR = IO0;
  delayMicroseconds(1);
  NRF_P0->OUTSET = CS;  // the flash enters deep power-down here. The pins stay driven at idle levels.
}

#ifdef USE_WATCHDOG
void startWatchdog() {
  // Keeps counting while the CPU sleeps, pauses while a debugger has it halted
  NRF_WDT->CONFIG = (WDT_CONFIG_SLEEP_Run << WDT_CONFIG_SLEEP_Pos) |
                    (WDT_CONFIG_HALT_Pause << WDT_CONFIG_HALT_Pos);
  NRF_WDT->CRV = WATCHDOG_MS * 32768 / 1000;
  NRF_WDT->RREN = WDT_RREN_RR0_Msk;
  NRF_WDT->TASKS_START = 1;
}

void feedWatchdog() {
  NRF_WDT->RR[0] = WDT_RR_RR_Reload;
}
#else
void startWatchdog() {}
void feedWatchdog() {}
#endif

#ifdef PRODUCTION
// Turn on readback protection (APPROTECT), so a debugger can't read the key out of flash.
// It takes effect from the next reset, so reboot once after setting it. Only a full chip erase
// (`nrfjprog --recover`), which also erases the key, turns it off again.
void enableReadbackProtection() {
  if ((NRF_UICR->APPROTECT & UICR_APPROTECT_PALL_Msk) == UICR_APPROTECT_PALL_Enabled) return;
  NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Wen << NVMC_CONFIG_WEN_Pos;
  while (!NRF_NVMC->READY) {}
  NRF_UICR->APPROTECT = 0xFFFFFF00;  // PALL = Enabled (flash bits can only be cleared)
  while (!NRF_NVMC->READY) {}
  NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Ren << NVMC_CONFIG_WEN_Pos;
  while (!NRF_NVMC->READY) {}
  NVIC_SystemReset();
}
#endif

// Firmware revision reported over BLE: "0.4.0" for production, "0.4.0-dev" otherwise. A production
// build on a board that still has the USB bootloader reports "0.4.0-bootloader", so the server can
// flag it: that bootloader lets anyone with a USB cable read the key or flash other firmware.
String firmwareRevision() {
#ifdef PRODUCTION
  // UICR.NRFFW[0] holds the bootloader address; it's erased (all 1s) when there is no bootloader
  if (NRF_UICR->NRFFW[0] != 0xFFFFFFFF) return FIRMWARE_VERSION "-bootloader";
  return FIRMWARE_VERSION;
#else
  return FIRMWARE_VERSION "-dev";
#endif
}

void setup() {
  // Fail-secure: make sure the solenoid is off before anything else
  pinMode(SOLENOID_PIN, OUTPUT);
  setLock(false);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);

  // DC/DC regulator instead of the LDO: about 30-40% less current with the CPU or radio on. The
  // XIAO has the inductor it needs (Zephyr's xiao_ble board runs REG1 in DC/DC mode).
  NRF_POWER->DCDCEN = 1;
  flashDeepPowerDown();

#ifdef PRODUCTION
  enableReadbackProtection();
  // The core starts USB before setup(). Production boards have no USB access, and with 5 V on the
  // USB/5V pin the USB peripheral would keep the high-frequency crystal running.
  PluggableUSBD().deinit();
#else
  Serial.begin(115200);
#endif
  startWatchdog();

  pinMode(PIN_VBAT_ENABLE, OUTPUT);
  digitalWrite(PIN_VBAT_ENABLE, LOW);
  analogReference(AR_INTERNAL2V4);
  analogAcquisitionTime(AT_40_US);  // the divider is high-impedance
  analogReadResolution(12);

#ifdef LATCH_PIN
  latchClosed = switchClosed(LATCH_PIN);
#endif

  if (!BLE.begin()) {
    LOGLN("Starting BLE failed, rebooting");
    delay(1000);
    NVIC_SystemReset();
  }

  BLE.setLocalName(LOCK_NAME);   // goes in the scan response (LOCK_SPEC.md, "Advertising")
  BLE.setDeviceName(LOCK_NAME);  // the GATT name, which iOS shows (and caches) after connecting
  BLE.setAdvertisedService(lockService);
  BLE.setAdvertisingInterval(ADVERTISING_INTERVAL);
  lockService.addCharacteristic(stateChar);
  lockService.addCharacteristic(uptimeChar);
  lockService.addCharacteristic(logChar);
  lockService.addCharacteristic(nonceChar);
  lockService.addCharacteristic(commandChar);
  BLE.addService(lockService);
  batteryService.addCharacteristic(batteryLevelChar);
  BLE.addService(batteryService);
  deviceInfoService.addCharacteristic(modelChar);
  deviceInfoService.addCharacteristic(firmwareRevChar);
  BLE.addService(deviceInfoService);

  modelChar.writeValue("HeloVT Lock");
  firmwareRevChar.writeValue(firmwareRevision());
  publishState();
  uptimeChar.writeValue(0);
  updateBattery();
  newNonce();

  logEvent(EVT_BOOT);

  BLE.advertise();
  LOGLN("Advertising as " LOCK_NAME ", firmware " + firmwareRevision());
}

// Everything that runs whether or not something is connected. Sleeps until a BLE event arrives
// or pollMs passes, so the CPU isn't busy all the time.
void service(unsigned long pollMs) {
  feedWatchdog();
  BLE.poll(pollMs);
  updateTimers();
  updateSensors();
  updateBattery();
}

void loop() {
  // Poll fast only while the relock timer is running. A connecting phone wakes BLE.poll() at once.
  service(unlocked ? POLL_MS : IDLE_POLL_MS);
  BLEDevice central = BLE.central();

  if (central) {
    logEvent(EVT_CONNECT);
    LOG("Connected: ");
    LOGLN(central.address());
    newNonce();  // a nonce read during an earlier connection can't be reused

    unsigned long lastUptimeSent = 0;
    unsigned long lastActivity = millis();
    while (central.connected()) {
      service(POLL_MS);
      if (commandChar.written()) {
        handleCommand(central);
        if (lastResult == RES_OK) lastActivity = millis();
      }

      if (millis() - lastActivity >= IDLE_MS) {
        LOGLN("Idle, disconnecting");
        central.disconnect();
        break;
      }

      // Push the board's internal time to the app once a second
      if (millis() - lastUptimeSent >= 1000) {
        lastUptimeSent = millis();
        uptimeChar.writeValue(lastUptimeSent);
      }
    }

    logEvent(EVT_DISCONNECT);
    newNonce();
  }
}
