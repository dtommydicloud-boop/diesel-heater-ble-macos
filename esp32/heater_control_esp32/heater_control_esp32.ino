/*
  heater_control_esp32.ino

  Control a Chinese diesel parking heater (AirHeaterBLE app protocol) over
  Bluetooth Low Energy, from an ESP32, acting as an always-on bridge -- it
  keeps trying to (re)connect on its own if the heater's not there yet or
  drops out, rather than giving up after one scan.

  UNVERIFIED ON REAL HARDWARE, and STILL UNVERIFIED AFTER TWO ROUNDS OF
  CODE REVIEW (not flash-testing). The protocol itself (command bytes, GATT
  UUIDs) is proven -- confirmed live against a real heater by the
  Python/bleak version in this repo. This ESP32 port has been through two
  independent AI code reviews (checked against the real Espressif
  arduino-esp32 BLE library source), which caught and fixed real API-misuse
  bugs a first pass missed -- but neither review is a substitute for
  actually flashing this to a board and testing it. Known remaining risk
  areas, flagged honestly rather than glossed over:
    - The non-blocking scan callback signature (`BLEScanResults` vs a
      pointer to it) has varied across arduino-esp32 core versions. If this
      doesn't compile against your installed core version, that's the
      first thing to check -- see the comment on onScanComplete() below.
    - Storing and reconnecting via a copied BLEAdvertisedDevice (to
      preserve the BLE address type, which a bare BLEAddress loses) assumes
      BLEAdvertisedDevice supports copy assignment safely. This has not
      been runtime-verified.
    - Response frame validation only checks for the AA 66 header seen in
      the two real captured responses (power on/off) -- the full response
      checksum algorithm is not confirmed, so this is a sanity check, not
      full validation.

  If you flash this and it works (or doesn't), please open an issue or PR
  -- that's genuinely the missing piece here, not more code review.

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
  connected. It scans/reconnects in the background on its own -- you
  shouldn't need to reset the board if the heater loses power or goes out
  of range temporarily (see caveats above on why this is "should" and not
  "confirmed").
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
static const uint32_t RECONNECT_RETRY_MS = 10000;  // how long to wait before starting another scan

// Advertised-name substrings seen in the wild for this protocol. Fallback
// only -- the primary match is the advertised GATT service UUID below,
// which is the real protocol-level identifier and (per independent review)
// the more reliable of the two, though not an absolute guarantee: the
// 16-bit FFE0/FFE1 UUID pattern is common enough in cheap BLE peripherals
// that a service-UUID match alone isn't mathematically unique to this
// heater family either -- it's confirmed by then successfully reading the
// specific characteristic and getting a real response, not by the
// advertisement alone.
static const char *KNOWN_NAME_HINTS[] = {"byd", "vevor", "airheater"};
static const int KNOWN_NAME_HINTS_COUNT = 3;

static BLEAdvertisedDevice targetDevice;  // copy-assigned in onResult(); avoids a raw new/delete per scan
static bool haveTarget = false;
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
  // Sanity check only -- the two real captured responses (power on/off)
  // both start AA 66, but the full response checksum/length rules aren't
  // confirmed the way the command-frame checksum is, so this is a soft
  // warning, not a hard validation.
  if (length < 2 || data[0] != 0xAA || data[1] != 0x66) {
    Serial.println("  (unexpected header -- may not be a valid heater response)");
  }
}

bool nameMatchesKnownHeater(const std::string &name) {
  std::string lower = name;
  for (auto &c : lower) c = tolower(c);
  for (int i = 0; i < KNOWN_NAME_HINTS_COUNT; i++) {
    if (lower.find(KNOWN_NAME_HINTS[i]) != std::string::npos) return true;
  }
  return false;
}

class ScanCallbacks : public BLEAdvertisedDeviceCallbacks {
  // Deliberately does NOT call BLEDevice::getScan()->stop() from here.
  // Stopping a scan from inside its own callback/interrupt context is a
  // known anti-pattern in the ESP32 BLE library that can deadlock the
  // scan mutex. Just record the match; the main loop stops the scan
  // safely, from outside this callback's context.
  void onResult(BLEAdvertisedDevice advertisedDevice) override {
    if (deviceFound) return;  // already have a match this scan cycle
    bool serviceMatch = advertisedDevice.isAdvertisingService(SERVICE_UUID);
    bool nameMatch = advertisedDevice.haveName() &&
                      nameMatchesKnownHeater(advertisedDevice.getName());
    if (serviceMatch || nameMatch) {
      Serial.print("Found candidate: ");
      Serial.print(advertisedDevice.haveName() ? advertisedDevice.getName().c_str() : "(no name)");
      Serial.print(" @ ");
      Serial.println(advertisedDevice.getAddress().toString().c_str());
      // Copy-assign rather than storing a bare BLEAddress -- preserves the
      // BLE address type (public/random), which BLEClient::connect() needs
      // for some peripherals and a bare address would silently drop.
      targetDevice = advertisedDevice;
      haveTarget = true;
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

// Fires when a scan completes, whether from timing out or being stopped
// via BLEScan::stop() from the main loop. This overload/signature matches
// the arduino-esp32 BLE library's non-blocking scan API -- if your
// installed core version has a different signature (some older/newer
// versions differ), this is the first thing to check if it fails to
// compile.
static void onScanComplete(BLEScanResults results) {
  scanning = false;
  if (!deviceFound) {
    // No match this cycle -- try again after a pause instead of spinning.
    lastReconnectAttemptMs = millis();
  }
}

void startScan() {
  if (scanning) return;
  Serial.println("Scanning for heater...");
  scanning = true;
  deviceFound = false;
  // Non-blocking overload (duration, completion-callback, continue-flag) --
  // NOT the blocking two-argument overload. Using the blocking one here
  // was a real bug in an earlier version of this file: it froze loop()
  // for the entire scan duration and, worse, left `scanning` stuck `true`
  // forever after a no-match scan, permanently breaking reconnection.
  BLEDevice::getScan()->start(SCAN_DURATION_S, onScanComplete, false);
}

bool connectToHeater() {
  if (!haveTarget) return false;

  // Disconnect and free any previous client before making a new one.
  // NOTE: an earlier version of this file called
  // BLEDevice::deleteClient(client), which does not exist in the current
  // official Espressif arduino-esp32 BLE library and would fail to
  // compile. Using plain `delete` on the client pointer instead, which is
  // the pattern the library itself expects.
  if (client != nullptr) {
    if (client->isConnected()) client->disconnect();
    delete client;
    client = nullptr;
  }

  client = BLEDevice::createClient();
  client->setClientCallbacks(&clientCallbacks);

  // Connecting via the BLEAdvertisedDevice* overload (not a bare address)
  // so the library can use the correct address type internally.
  if (!client->connect(&targetDevice)) {
    Serial.println("Connect failed.");
    delete client;
    client = nullptr;
    return false;
  }
  BLERemoteService *service = client->getService(SERVICE_UUID);
  if (service == nullptr) {
    Serial.println("Service not found -- is this really the heater?");
    client->disconnect();
    delete client;
    client = nullptr;
    return false;
  }
  remoteChar = service->getCharacteristic(CHAR_UUID);
  if (remoteChar == nullptr) {
    Serial.println("Characteristic not found.");
    client->disconnect();
    delete client;
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
  bool ok = remoteChar->writeValue(frame, 8, true);
  if (!ok) {
    Serial.println("Write failed -- command may not have been sent.");
  }
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
    // Safe to call stop() here -- this is the MAIN LOOP, not the scan
    // callback itself, so it doesn't hit the deadlock anti-pattern.
    // Calling stop() triggers onScanComplete(), which clears `scanning`.
    BLEDevice::getScan()->stop();
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
