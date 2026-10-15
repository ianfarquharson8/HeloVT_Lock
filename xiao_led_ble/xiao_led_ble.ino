// XIAO nRF52840 - BLE-controlled onboard LED
// Board package: "Seeed nRF52 mbed-enabled Boards" -> "Seeed XIAO BLE - nRF52840"
// Library: ArduinoBLE
//
// Write 0x01 to the LED characteristic to turn the LED on and drive D0 HIGH (3.3V),
// 0x00 to turn both off.
//
// The board also keeps a log of BOOT / CONNECT / DISCONNECT events, timestamped with
// millis() (time since power-on). The log is lost on power-off/reset.
//   - Uptime characteristic: uint32 ms since boot, notified once per second while connected.
//   - Log characteristic: oldest-first list of 5-byte entries
//       [type (0=boot, 1=connect, 2=disconnect)][uint32 ms, little-endian]
//
// NOTE: D0 can only source a few mA. It's fine for a multimeter test, but a solenoid
// must be driven through a MOSFET/transistor (with a flyback diode), not directly from D0.

#include <ArduinoBLE.h>

// These UUIDs must match the ones in laptop_app.py and web/index.html
#define SERVICE_UUID "19b10000-e8f2-537e-4f6c-d104768a1214"
#define LED_CHAR_UUID "19b10001-e8f2-537e-4f6c-d104768a1214"
#define UPTIME_CHAR_UUID "19b10002-e8f2-537e-4f6c-d104768a1214"
#define LOG_CHAR_UUID "19b10003-e8f2-537e-4f6c-d104768a1214"

// Keep the most recent MAX_EVENTS events (5 bytes each, must fit in 512 bytes)
const int MAX_EVENTS = 100;
const int EVENT_SIZE = 5;

enum EventType : uint8_t { EVT_BOOT = 0, EVT_CONNECT = 1, EVT_DISCONNECT = 2 };

BLEService ledService(SERVICE_UUID);
BLEByteCharacteristic ledChar(LED_CHAR_UUID, BLERead | BLEWrite);
BLEUnsignedLongCharacteristic uptimeChar(UPTIME_CHAR_UUID, BLERead | BLENotify);
BLECharacteristic logChar(LOG_CHAR_UUID, BLERead, MAX_EVENTS * EVENT_SIZE);

// The XIAO's onboard LEDs are active LOW (LOW = on)
const int LED_PIN = LED_BUILTIN;

// Solenoid control output (active HIGH: HIGH = 3.3V)
const int SOLENOID_PIN = D0;

// Ring buffer of events
uint8_t eventTypes[MAX_EVENTS];
uint32_t eventTimes[MAX_EVENTS];
int eventStart = 0;
int eventCount = 0;

void setLed(bool on) {
  digitalWrite(LED_PIN, on ? LOW : HIGH);
  digitalWrite(SOLENOID_PIN, on ? HIGH : LOW);
}

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

  const char* names[] = {"BOOT", "CONNECT", "DISCONNECT"};
  Serial.print("[");
  Serial.print(now);
  Serial.print(" ms] ");
  Serial.println(names[type]);
}

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  pinMode(SOLENOID_PIN, OUTPUT);
  setLed(false);

  if (!BLE.begin()) {
    Serial.println("Starting BLE failed!");
    while (1);
  }

  BLE.setLocalName("XIAO-LED");
  BLE.setAdvertisedService(ledService);
  ledService.addCharacteristic(ledChar);
  ledService.addCharacteristic(uptimeChar);
  ledService.addCharacteristic(logChar);
  BLE.addService(ledService);
  ledChar.writeValue(0);
  uptimeChar.writeValue(0);

  logEvent(EVT_BOOT);

  BLE.advertise();
  Serial.println("Advertising as XIAO-LED...");
}

void loop() {
  BLEDevice central = BLE.central();

  if (central) {
    logEvent(EVT_CONNECT);
    Serial.print("Connected: ");
    Serial.println(central.address());

    unsigned long lastUptimeSent = 0;
    while (central.connected()) {
      if (ledChar.written()) {
        bool on = ledChar.value() != 0;
        setLed(on);
        Serial.println(on ? "LED ON, D0 HIGH" : "LED OFF, D0 LOW");
      }

      // Push the board's internal time to the app once a second
      if (millis() - lastUptimeSent >= 1000) {
        lastUptimeSent = millis();
        uptimeChar.writeValue(lastUptimeSent);
      }
    }

    logEvent(EVT_DISCONNECT);
    setLed(false);
  }
}
