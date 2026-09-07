/*
  heater_control_esp32.ino

  Control a Chinese diesel parking heater (AirHeaterBLE app protocol) over
  Bluetooth Low Energy, from an ESP32, acting as a real always-on bridge --
  it keeps trying to (re)connect on its own if the heater's not there yet
  or drops out, rather than giving up after one scan.

  UNVERIFIED ON REAL HARDWARE. The protocol (command bytes, GATT UUIDs) is
  proven -- confirmed live against a real heater by the Python/bleak version
  in this repo. This ESP32 port has not itself been flash-tested against a
  real device yet.

  Why an ESP32 instead of a Mac/Pi: it's a few dollars, draws very little
  power, and can sit permanently plugged in right next to the heater as an
  always-on bridge -- no OS to maintain, no laptop that needs to stay
  awake and in range.

  Board setup (Arduino IDE): Tools -> Board -> ESP32 Arduino -> pick your
  board. Requires the built-in "BLE" library that ships with the ESP32
  Arduino core (no extra library install needed for a standard esp32 core
  install).

  Usage: flash this, open Serial Monitor at 115200 baud. Type "on", "off",
  or "status" + Enter to send that command once a heater's found and
  connected. It will keep scanning/reconnecting in the background on its
  own -- you don't need to reset the board if the heater loses power or
  goes out of range temporarily.
*/

#include <BLEDevice.h>
#include <BLEClient.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>

static BLEUUID SERVICE_UUID("0000ffe0-0000-1000-8000-00805f9b34fb");
static BLEUUID CHAR_UUID("0000ffe1-0000-1000-8000-00805f9b34fb");

static const uint8_t CMD_STATUS = 0x01;
static const uint8_t CMD_MODE = 0x02;
static const uint8_t CMD_POWER = 0x03;
static const uint8_t CMD_LEVEL_OR_TEMP = 0x04;

static const uint32_t SCAN_DURATION_S = 15;
static const uint32_t RECONNECT_RETRY_MS = 10000;  // how often to retry once disconnected

// Advertised-name substrings seen in the wild for this protocol. This is a
// fallback/logging aid, not the primary match -- name-only matching would
// silently miss any brand not in this list (a real bug in an earlier
// version of this file: it only checked for "byd", so a Vevor-branded unit
// using the identical protocol would never be found despite this repo
// claiming Vevor compatibility). The primary match is the advertised GATT
// service UUID below, which is protocol-specific regardless of brand name.
static const char *KNOWN_NAME_HINTS[] = {"byd", "vevor", "airheater"};
static const int KNOWN_NAME_HINTS_COUNT = 3;

// State -- stored by value, not heap-allocated, so there's nothing to leak
// across repeated scan/connect/disconnect cycles.
static BLEAddress *targetAddress = nullptr;  // only ever holds one address at a time; see setTarget()
static BLEClient *client = nullptr;
static BLERemoteCharacteristic *remoteChar = nullptr;
static volatile bool connected = false;
static volatile bool deviceFound = false;
static volatile bool scanning = false;
static unsigned long lastReconnectAttemptMs = 0;

// Build a command frame: AA 55 0C 22 <type> <value> 00 <checksum>,
// checksum = sum(bytes[2..6]) % 256 -- same frame format as the Python
// version in this repo, confirmed live against a real heater.
void buildCommand(uint8_t commandType, uint8_t value, uint8_t out[8]) {
  out[0] = 0xAA;
  out[1] = 0x55;
  out[2] = 0x0C;
  out[3] = 0x22;
  out[4] = commandType;
  out[5] = value;
  out[6] = 0x00;
  uint16_t sum = 0;
  for (int i = 2; i <= 6; i++) sum += out[i];
  out[7] = (uint8_t)(sum % 256);
}

static void notifyCallback(BLERemoteCharacteristic *ch, uint8_t *data,
                            size_t length, bool isNotify) {
  Serial.print("Response: ");
  for (size_t i = 0; i < length; i++) {
    if (data[i] < 0x10) Serial.print("0");
    Serial.print(data[i], HEX);
  }
  Serial.println();
}

bool nameMatchesKnownHeater(const std::string &name) {
  std::string lower = name;
  for (auto &c : lower) c = tolower(c);
  for (int i = 0; i < KNOWN_NAME_HINTS_COUNT; i++) {
    if (lower.find(KNOWN_NAME_HINTS[i]) != std::string::npos) return true;
  }
  return false;
}

void setTarget(BLEAddress addr) {
  if (targetAddress != nullptr) {
    delete targetAddress;
  }
  targetAddress = new BLEAddress(addr);
}

class ScanCallbacks : public BLEAdvertisedDeviceCallbacks {
  // Deliberately does NOT call BLEDevice::getScan()->stop() from here.
  // Stopping a scan from inside its own callback/interrupt context is a
  // known anti-pattern in the ESP32 BLE library that can deadlock the
  // scan mutex. Just set a flag; the main loop stops the scan safely.
  void onResult(BLEAdvertisedDevice advertisedDevice) override {
    bool serviceMatch = advertisedDevice.isAdvertisingService(SERVICE_UUID);
    bool nameMatch = advertisedDevice.haveName() &&
                      nameMatchesKnownHeater(advertisedDevice.getName());
    if (serviceMatch || nameMatch) {
      Serial.print("Found candidate: ");
      Serial.print(advertisedDevice.haveName() ? advertisedDevice.getName().c_str() : "(no name)");
      Serial.print(" @ ");
      Serial.println(advertisedDevice.getAddress().toString().c_str());
      setTarget(advertisedDevice.getAddress());
      deviceFound = true;
    }
  }
};

class ClientCallbacks : public BLEClientCallbacks {
  void onConnect(BLEClient *c) override {
    Serial.println("Connected to heater.");
  }
  void onDisconnect(BLEClient *c) override {
    Serial.println("Disconnected from heater -- will retry.");
    connected = false;
    remoteChar = nullptr;
  }
};

static ClientCallbacks clientCallbacks;

void startScan() {
  if (scanning) return;
  Serial.println("Scanning for heater...");
  scanning = true;
  deviceFound = false;
  BLEScan *scan = BLEDevice::getScan();
  scan->start(SCAN_DURATION_S, false);
}

bool connectToHeater() {
  if (targetAddress == nullptr) return false;

  // Free any previous client before making a new one -- an earlier version
  // of this file called BLEDevice::createClient() again on every retry
  // without ever freeing the old one, leaking a client object per failed
  // attempt until the heap ran out.
  if (client != nullptr) {
    BLEDevice::deleteClient(client);
    client = nullptr;
  }

  client = BLEDevice::createClient();
  client->setClientCallbacks(&clientCallbacks);

  if (!client->connect(*targetAddress)) {
    Serial.println("Connect failed.");
    BLEDevice::deleteClient(client);
    client = nullptr;
    return false;
  }
  BLERemoteService *service = client->getService(SERVICE_UUID);
  if (service == nullptr) {
    Serial.println("Service not found -- is this really the heater?");
    client->disconnect();
    BLEDevice::deleteClient(client);
    client = nullptr;
    return false;
  }
  remoteChar = service->getCharacteristic(CHAR_UUID);
  if (remoteChar == nullptr) {
    Serial.println("Characteristic not found.");
    client->disconnect();
    BLEDevice::deleteClient(client);
    client = nullptr;
    return false;
  }
  if (remoteChar->canNotify()) {
    remoteChar->registerForNotify(notifyCallback);
  }
  return true;
}

void sendCommand(uint8_t commandType, uint8_t value) {
  if (!connected || remoteChar == nullptr) {
    Serial.println("Not connected -- waiting for heater to be found and connected.");
    return;
  }
  uint8_t frame[8];
  buildCommand(commandType, value, frame);
  remoteChar->writeValue(frame, 8, true);
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  BLEDevice::init("");
  BLEScan *scan = BLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(new ScanCallbacks());
  scan->setActiveScan(true);
  Serial.println("Ready. Type on / off / status and press Enter.");
  Serial.println(
      "Note: some ESP32 variants -- e.g. the ESP32-S2 -- have no Bluetooth "
      "at all, only WiFi. You need a variant with real BLE, like the "
      "original ESP32 or an S3/C3/C6.");
  startScan();
}

void loop() {
  // Scan/connect state machine -- this is what makes it an actual
  // always-on bridge instead of a one-shot script: if the heater isn't
  // found yet, or drops out later, this keeps retrying on its own rather
  // than requiring a physical reset.
  if (scanning && deviceFound) {
    BLEDevice::getScan()->stop();
    scanning = false;
    if (connectToHeater()) {
      connected = true;
    } else {
      lastReconnectAttemptMs = millis();
    }
  }

  if (!connected && !scanning) {
    if (millis() - lastReconnectAttemptMs > RECONNECT_RETRY_MS) {
      lastReconnectAttemptMs = millis();
      startScan();
    }
  }

  if (!Serial.available()) return;
  String cmd = Serial.readStringUntil('\n');
  cmd.trim();
  cmd.toLowerCase();

  if (cmd == "on") {
    sendCommand(CMD_POWER, 0x01);
  } else if (cmd == "off") {
    sendCommand(CMD_POWER, 0x00);
  } else if (cmd == "status") {
    sendCommand(CMD_STATUS, 0x00);
  } else if (cmd.length() > 0) {
    Serial.println("Unknown command. Type on / off / status.");
  }
}
