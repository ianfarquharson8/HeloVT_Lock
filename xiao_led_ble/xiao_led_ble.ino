// HeloVT lock - XIAO nRF52840 firmware: BLE-controlled lock (solenoid on D0, onboard LED mirrors it)
// Board package: "Seeed nRF52 mbed-enabled Boards" -> "Seeed XIAO BLE - nRF52840"
// Library: ArduinoBLE
// Before uploading, run `python make_key.py` in the project folder to create secret.h.
//
// SECURITY: commands must be signed with a secret key shared by the board and the app
// (HMAC-SHA256 challenge-response). The key never goes over the air.
//   1. App reads the Nonce characteristic: 16 random bytes, valid for one command.
//   2. App writes the Command characteristic, 33 bytes:
//        [cmd][HMAC-SHA256(key, nonce || cmd)]    cmd: 0x01 = unlock, 0x00 = lock now
//   3. Board checks the HMAC, acts on it, picks a new nonce, then notifies State.
// The nonce changes after every attempt and on every connect, so a recorded command can't be
// replayed. After MAX_FAILS bad signatures in a row, the board drops the connection and ignores
// commands for LOCKOUT_MS. An unlock relocks by itself after UNLOCK_MS.
//
// Characteristics (service SERVICE_UUID):
//   State   (read/notify): [lock state: 0 = locked, 1 = unlocked][result of last command: CmdResult]
//   Uptime  (read/notify): uint32 ms since boot, notified once per second while connected.
//   Log     (read): oldest-first list of 5-byte entries [type: EventType][uint32 ms, little-endian]
//   Nonce   (read): 16 bytes
//   Command (write): 33 bytes, as above
// The log is kept in RAM and is lost on power-off/reset.
//
// The BLE link itself is not encrypted, so anyone nearby can read State, Uptime and Log.
// They can't unlock without the key.
//
// NOTE: D0 can only source a few mA. It's fine for a multimeter test, but a solenoid
// must be driven through a MOSFET/transistor (with a flyback diode), not directly from D0.

#include <ArduinoBLE.h>
#include <utility/HCI.h>
#include "hmac_sha256.h"

#if __has_include("secret.h")
#include "secret.h"  // defines LOCK_KEY
#else
#error "secret.h is missing. Run `python make_key.py` in the project folder to create it."
#endif

// These must match laptop_app.py and web/index.html
#define DEVICE_NAME "HeloVT-Lock"
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
const int MAX_FAILS = 3;                 // bad signatures in a row before a lockout
const unsigned long LOCKOUT_MS = 30000;

enum Command : uint8_t { CMD_LOCK = 0, CMD_UNLOCK = 1 };
enum CmdResult : uint8_t { RES_NONE = 0, RES_OK = 1, RES_BAD_AUTH = 2, RES_LOCKED_OUT = 3, RES_BAD_FORMAT = 4 };

// Keep the most recent MAX_EVENTS events (5 bytes each, must fit in 512 bytes)
const int MAX_EVENTS = 100;
const int EVENT_SIZE = 5;

enum EventType : uint8_t {
  EVT_BOOT = 0, EVT_CONNECT = 1, EVT_DISCONNECT = 2, EVT_UNLOCK = 3, EVT_LOCK = 4, EVT_AUTH_FAIL = 5
};

BLEService lockService(SERVICE_UUID);
BLECharacteristic stateChar(STATE_CHAR_UUID, BLERead | BLENotify, 2);
BLEUnsignedLongCharacteristic uptimeChar(UPTIME_CHAR_UUID, BLERead | BLENotify);
BLECharacteristic logChar(LOG_CHAR_UUID, BLERead, MAX_EVENTS * EVENT_SIZE);
BLECharacteristic nonceChar(NONCE_CHAR_UUID, BLERead, NONCE_LEN);
BLECharacteristic commandChar(COMMAND_CHAR_UUID, BLEWrite, COMMAND_LEN);

// The XIAO's onboard LEDs are active LOW (LOW = on)
const int LED_PIN = LED_BUILTIN;

// Solenoid control output (active HIGH: HIGH = 3.3V)
const int SOLENOID_PIN = D0;

bool unlocked = false;
unsigned long unlockedAt = 0;
uint8_t lastResult = RES_NONE;

uint8_t nonce[NONCE_LEN];
int failCount = 0;
bool lockedOut = false;
unsigned long lockoutStart = 0;

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

  const char* names[] = {"BOOT", "CONNECT", "DISCONNECT", "UNLOCK", "LOCK", "AUTH FAIL"};
  Serial.print("[");
  Serial.print(now);
  Serial.print(" ms] ");
  Serial.println(names[type]);
}

void publishState() {
  uint8_t value[2] = {unlocked ? (uint8_t)1 : (uint8_t)0, lastResult};
  stateChar.writeValue(value, sizeof(value));
}

void setLock(bool unlock) {
  unlocked = unlock;
  unlockedAt = millis();
  digitalWrite(LED_PIN, unlock ? LOW : HIGH);
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
  if (!nonceValid) Serial.println("Could not get random bytes for the nonce");
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

  if (lockedOut) {
    lastResult = RES_LOCKED_OUT;
  } else if (!nonceValid || len != COMMAND_LEN || (msg[0] != CMD_LOCK && msg[0] != CMD_UNLOCK)) {
    lastResult = RES_BAD_FORMAT;
  } else if (!signatureValid(msg[0], msg + 1)) {
    lastResult = RES_BAD_AUTH;
  } else {
    lastResult = RES_OK;
  }

  // Every attempt uses up the nonce. Set the new one before notifying, so the app
  // reads a fresh nonce for its next command.
  newNonce();

  if (lastResult == RES_OK) {
    failCount = 0;
    setLock(msg[0] == CMD_UNLOCK);
    logEvent(unlocked ? EVT_UNLOCK : EVT_LOCK);
  } else if (lastResult == RES_BAD_AUTH) {
    logEvent(EVT_AUTH_FAIL);
    if (++failCount >= MAX_FAILS) {
      failCount = 0;
      lockedOut = true;
      lockoutStart = millis();
      lastResult = RES_LOCKED_OUT;
      Serial.println("Too many bad commands, locked out");
      publishState();
      central.disconnect();
      return;
    }
  }
  publishState();
}

// Auto-relock and lockout expiry. Runs whether or not anything is connected.
void updateTimers() {
  if (unlocked && millis() - unlockedAt >= UNLOCK_MS) {
    setLock(false);
    logEvent(EVT_LOCK);
    publishState();
  }
  if (lockedOut && millis() - lockoutStart >= LOCKOUT_MS) {
    lockedOut = false;
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  pinMode(SOLENOID_PIN, OUTPUT);
  setLock(false);

  if (!BLE.begin()) {
    Serial.println("Starting BLE failed!");
    while (1);
  }

  BLE.setLocalName(DEVICE_NAME);
  BLE.setAdvertisedService(lockService);
  lockService.addCharacteristic(stateChar);
  lockService.addCharacteristic(uptimeChar);
  lockService.addCharacteristic(logChar);
  lockService.addCharacteristic(nonceChar);
  lockService.addCharacteristic(commandChar);
  BLE.addService(lockService);
  publishState();
  uptimeChar.writeValue(0);
  newNonce();

  logEvent(EVT_BOOT);

  BLE.advertise();
  Serial.println("Advertising as " DEVICE_NAME "...");
}

void loop() {
  updateTimers();
  BLEDevice central = BLE.central();

  if (central) {
    logEvent(EVT_CONNECT);
    Serial.print("Connected: ");
    Serial.println(central.address());
    newNonce();  // a nonce read during an earlier connection can't be reused

    unsigned long lastUptimeSent = 0;
    while (central.connected()) {
      updateTimers();
      if (commandChar.written()) handleCommand(central);

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
