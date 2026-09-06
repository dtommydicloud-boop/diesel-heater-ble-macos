/*
  heater_control_esp32.ino

  Control a Chinese diesel parking heater (BYD-branded, AirHeaterBLE app
  protocol) over Bluetooth Low Energy, from an ESP32.

  UNVERIFIED ON REAL HARDWARE. The protocol (command bytes, GATT UUIDs) is
  proven -- confirmed live against a real heater by the Python/bleak version
  in this repo. This ESP32 port has not itself been flash-tested against a
  real device yet. The BLE client logic follows the standard ESP32 Arduino
  BLE library patterns; if something doesn't compile or connect on your
  board revision, that's the part to debug first, not the protocol.

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
  connected.
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

static BLEAdvertisedDevice *foundDevice = nullptr;
static BLEClient *client = nullptr;
static BLERemoteCharacteristic *remoteChar = nullptr;
static bool connected = false;

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

class ScanCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) override {
    String name = advertisedDevice.getName().c_str();
    String nameLower = name;
    nameLower.toLowerCase();
    if (nameLower.indexOf("byd") >= 0) {
      Serial.print("Found heater: ");
      Serial.println(name);
      foundDevice = new BLEAdvertisedDevice(advertisedDevice);
      BLEDevice::getScan()->stop();
    }
  }
};

bool connectToHeater() {
  client = BLEDevice::createClient();
  if (!client->connect(foundDevice)) {
    Serial.println("Connect failed.");
    return false;
  }
  BLERemoteService *service = client->getService(SERVICE_UUID);
  if (service == nullptr) {
    Serial.println("Service not found -- is this really the heater?");
    client->disconnect();
    return false;
  }
  remoteChar = service->getCharacteristic(CHAR_UUID);
  if (remoteChar == nullptr) {
    Serial.println("Characteristic not found.");
    client->disconnect();
    return false;
  }
  if (remoteChar->canNotify()) {
    remoteChar->registerForNotify(notifyCallback);
  }
  Serial.println("Connected to heater.");
  return true;
}

void sendCommand(uint8_t commandType, uint8_t value) {
  if (!connected || remoteChar == nullptr) {
    Serial.println("Not connected -- scan/connect first.");
    return;
  }
  uint8_t frame[8];
  buildCommand(commandType, value, frame);
  remoteChar->writeValue(frame, 8, true);
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("Scanning for heater (BLE name containing 'byd')...");

  BLEDevice::init("");
  BLEScan *scan = BLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(new ScanCallbacks());
  scan->setActiveScan(true);
  scan->start(15, false);  // 15s scan, matches the Python version's timeout

  if (foundDevice == nullptr) {
    Serial.println(
        "Heater not found in 15s. Check it's powered and in range, and "
        "that this board's Bluetooth is actually working (some ESP32 "
        "variants -- e.g. the ESP32-S2 -- have no Bluetooth at all, only "
        "WiFi; you need a variant with real BLE, like the original ESP32 "
        "or an S3/C3/C6).");
    return;
  }

  connected = connectToHeater();
  if (connected) {
    Serial.println("Ready. Type on / off / status and press Enter.");
  }
}

void loop() {
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
