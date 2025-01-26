#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <WiFiManager.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <NimBLEDevice.h>
#include "constants.h"
#include <Fonts/Picopixel.h>

WiFiClient wifiClient;
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
#define OLED_I2C_ADDRESS 0x3C

WiFiManager wifiManager;
int reconnections = 0;
String ssidNew;
String passNew;
String sessionID;
StaticJsonDocument<256> doc;

static const NimBLEAdvertisedDevice* advDevice;
static bool doConnect = false;
static uint32_t scanTimeMs = 10000;

class ScanCallbacks : public NimBLEScanCallbacks {
    void onResult(const NimBLEAdvertisedDevice* advertisedDevice) override {
      Serial.printf("Found BLE device: %s\n", advertisedDevice->toString().c_str());
      if (advertisedDevice->getName() == "AT-02") {
        Serial.println("Target BLE device found.");
        NimBLEDevice::getScan()->stop();
        advDevice = advertisedDevice;
        doConnect = true;
      }
    }
} scanCallbacks;

void startScan() {
  NimBLEScan* pScan = NimBLEDevice::getScan();
  pScan->setScanCallbacks(&scanCallbacks, false);
  pScan->setActiveScan(true);
  pScan->start(scanTimeMs, false);
}
class ClientCallbacks : public NimBLEClientCallbacks {
    void onConnect(NimBLEClient* pClient) override {
      Serial.println("Connected to BLE device.");
    }
    void onDisconnect(NimBLEClient* pClient, int reason) override {
      Serial.printf("Disconnected from BLE, reason: %d. Restarting scan...\n", reason);
      doConnect = false; // Reset connection flag
      advDevice = nullptr; // Reset advertised device
      startScan(); // Restart scanning
    }
};


float temperatures[4] = {0, 0, 0, 0};

void notifyCallback(NimBLERemoteCharacteristic* characteristic, uint8_t* data, size_t length, bool isNotify) {
  Serial.print("Notification received: ");
  for (size_t i = 0; i < length; i++) {
    Serial.printf("%02X ", data[i]);
  }
  Serial.println();

  for (int i = 0; i < 4; i++) {
    int highByte = data[5 + i * 2];
    int lowByte = data[6 + i * 2];
    temperatures[i] = (highByte << 8) | lowByte;

    temperatures[i] = (temperatures[i] == 65535) ? 0 : temperatures[i] / 10;

    Serial.printf("Temperature %d: %.2f\n", i + 1, (float)temperatures[i]);
  }
}

bool connectToDevice() {
  NimBLEClient* pClient = nullptr;

  if (NimBLEDevice::getCreatedClientCount()) {
    pClient = NimBLEDevice::getClientByPeerAddress(advDevice->getAddress());
    if (pClient && !pClient->connect(advDevice)) {
      Serial.println("Reconnect to BLE device failed.");
      return false;
    }
  }

  if (!pClient) {
    if (NimBLEDevice::getCreatedClientCount() >= NIMBLE_MAX_CONNECTIONS) {
      Serial.println("Max BLE clients reached.");
      return false;
    }

    pClient = NimBLEDevice::createClient();
    pClient->setClientCallbacks(new ClientCallbacks(), false);

    if (!pClient->connect(advDevice)) {
      NimBLEDevice::deleteClient(pClient);
      Serial.println("Failed to connect to BLE device.");
      advDevice = nullptr;
      doConnect = false;
      startScan(); // Restart scanning
      return false;
    }
  }

  Serial.println("Connected to BLE device.");

  NimBLERemoteService* pService = pClient->getService("0000CEE0-0000-1000-8000-00805F9B34FB");
  if (!pService) {
    Serial.println("Failed to find BLE service.");
    return false;
  }
  NimBLERemoteCharacteristic* pCharacteristicCEE1 = pService->getCharacteristic("0000CEE1-0000-1000-8000-00805F9B34FB");
  NimBLERemoteCharacteristic* pCharacteristicCEE2 = pService->getCharacteristic("0000CEE2-0000-1000-8000-00805F9B34FB");
  if (!pCharacteristicCEE2 || !pCharacteristicCEE2->canNotify()) {
    Serial.println("Failed to find BLE characteristic or it does not support notifications.");
    return false;
  }

  uint8_t authCode1[] = {0x55, 0xAA, 0x00, 0x03, 0xA0, 0x00, 0x00, 0x5C};
  uint8_t authCode2[] = {0x55, 0xAA, 0x00, 0x02, 0xA1, 0x00, 0x5C};

  pCharacteristicCEE1->writeValue((const uint8_t*)authCode1, sizeof(authCode1));
  delay(200);
  pCharacteristicCEE1->writeValue((const uint8_t*)authCode2, sizeof(authCode2));


  pCharacteristicCEE2->subscribe(true, notifyCallback);
  Serial.println("Subscribed to BLE notifications.");

  return true;
}

void setup() {
  Serial.begin(115200);
  Serial.println("Starting system...");

  Wire.begin(5, 4);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_I2C_ADDRESS)) {
    Serial.println("OLED initialization failed.");
    while (true) delay(1000);
  }

  display.clearDisplay();
  display.setFont(&Picopixel);
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 5);

  display.println("Connecting...");
  display.display();

  wifiManager.autoConnect("SmokerAP");
  ssidNew = WiFi.SSID();
  passNew = WiFi.psk();
  Serial.println("WiFi connected: " + ssidNew);

  display.println("WiFi Connected!");
  display.display();
  sessionID = initSessionOnCloud();

  NimBLEDevice::init("");
  NimBLEDevice::setSecurityAuth(false, false, true);

  startScan();
  //  NimBLEScan* pScan = NimBLEDevice::getScan();
  //  pScan->setScanCallbacks(&scanCallbacks, false);
  //  pScan->setActiveScan(true);
  //  pScan->start(scanTimeMs, false);
}

int16_t counter = 0;
float t1sum = 0, t2sum = 0, t3sum = 0, t4sum = 0;

void loop() {
  if (doConnect) {
    doConnect = false;
    if (!connectToDevice()) {
      Serial.println("Failed to connect to BLE device.");
    }
  }

  display.setCursor(0, 10);
  display.clearDisplay();

  t1sum += temperatures[0];
  t2sum += temperatures[1];
  t3sum += temperatures[2];
  t4sum += temperatures[3];

  printTemp(" T1: ", temperatures[0]);
  printTemp(" T2: ", temperatures[1]);
  display.setCursor(62, 10);
  printTemp(" T3: ", temperatures[2]);
  display.setCursor(62, 30);
  printTemp(" T4: ", temperatures[3]);

  display.setTextSize(1);
  display.println(String(reconnections, 1));
  display.display();

  if (counter >= 16) {
    String payload = updateDataToCloud(t1sum / 16, t2sum / 16, t3sum / 16, t4sum / 16);
    t1sum = t2sum = t3sum = t4sum = 0;
    counter = 0;

    DeserializationError error = deserializeJson(doc, payload);
    if (error) {
      Serial.println("Failed to parse server response.");
    } else {
      int id = doc["id"];
      Serial.println("Server response ID: " + String(id));
    }
  }

  counter++;
  delay(500);
}

void printTemp(String label, float t) {
  display.setTextSize(1);
  display.print(label);
  display.setTextSize(3);
  display.println(String(t, 1));
}

String initSessionOnCloud() {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    String url = "http://" + BASE_URL + "/initSession";
    http.begin(wifiClient, url);
    int httpCode = http.GET();
    String payload = http.getString();
    http.end();
    return payload;
  }
  WiFi.begin(ssidNew.c_str(), passNew.c_str());
  reconnections++;
  return "";
}

String updateDataToCloud(float t1, float t2, float t3, float t4) {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    String url = "http://" + BASE_URL + "/multiTempUpdate?t1=" + String(t1) + "&t2=" + String(t2) + "&t3=" + String(t3) + "&t4=" + String(t4) + "&sessionID=" + sessionID;
    http.begin(wifiClient, url);
    int httpCode = http.GET();
    String payload = http.getString();
    http.end();
    return payload;
  }
  WiFi.begin(ssidNew.c_str(), passNew.c_str());
  reconnections++;
  return "";
}
