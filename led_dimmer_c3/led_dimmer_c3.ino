/*
 * ESP32-C3 SuperMini - BLE 5V LED dimmer + battery monitor
 * Board: ESP32C3 Dev Module (esp32 3.x), USB CDC On Boot: Enabled
 *
 * Wiring:
 *   GPIO0 -> 220R -> IRLB8721 Gate (10k pull-down to GND), Drain -> LED-, Source -> GND
 *   GPIO1 -> 100k/100k divider from BAT+ (+ 100nF to GND)
 */

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

#define LED_PIN        0
#define BAT_PIN        1
#define PWM_FREQ       5000
#define PWM_BITS       10
#define DIVIDER_RATIO  2.0f
#define BAT_INTERVAL   5000
#define USE_GAMMA      true

#define DEVICE_NAME    "LED-Dimmer"

// Nordic UART Service
#define SERVICE_UUID   "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHAR_RX_UUID   "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHAR_TX_UUID   "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

BLEServer*         server  = nullptr;
BLECharacteristic* txChar  = nullptr;
bool               connected = false;
int                brightness = 0;
unsigned long      lastBat = 0;

void pwmInit() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(LED_PIN, PWM_FREQ, PWM_BITS);
#else
  ledcSetup(0, PWM_FREQ, PWM_BITS);
  ledcAttachPin(LED_PIN, 0);
#endif
}

void pwmWrite(uint32_t duty) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(LED_PIN, duty);
#else
  ledcWrite(0, duty);
#endif
}

void setBrightness(int percent) {
  brightness = constrain(percent, 0, 100);
  float x = brightness / 100.0f;
  if (USE_GAMMA) x = powf(x, 2.2f);
  pwmWrite((uint32_t)(x * ((1 << PWM_BITS) - 1) + 0.5f));
}

float readBatteryVoltage() {
  uint32_t sum = 0;
  for (int i = 0; i < 16; i++) {
    sum += analogReadMilliVolts(BAT_PIN);
    delay(2);
  }
  return (sum / 16.0f) / 1000.0f * DIVIDER_RATIO;
}

int batteryPercent(float v) {
  static const float tbl[][2] = {
    {4.20, 100}, {4.15, 95}, {4.11, 90}, {4.08, 85}, {4.02, 80},
    {3.98, 75},  {3.95, 70}, {3.91, 65}, {3.87, 60}, {3.85, 55},
    {3.84, 50},  {3.82, 45}, {3.80, 40}, {3.79, 35}, {3.77, 30},
    {3.75, 25},  {3.73, 20}, {3.71, 15}, {3.69, 10}, {3.61, 5},
    {3.27, 0}
  };
  const int n = sizeof(tbl) / sizeof(tbl[0]);
  if (v >= tbl[0][0]) return 100;
  if (v <= tbl[n - 1][0]) return 0;
  for (int i = 0; i < n - 1; i++) {
    if (v <= tbl[i][0] && v > tbl[i + 1][0]) {
      float k = (v - tbl[i + 1][0]) / (tbl[i][0] - tbl[i + 1][0]);
      return (int)(tbl[i + 1][1] + k * (tbl[i][1] - tbl[i + 1][1]) + 0.5f);
    }
  }
  return 0;
}

void sendText(const String& s) {
  if (!connected || !txChar) return;
  txChar->setValue((uint8_t*)s.c_str(), s.length());
  txChar->notify();
}

void sendBattery() {
  float v = readBatteryVoltage();
  int p = batteryPercent(v);
  String s = "Battery: " + String(v, 2) + " V (" + String(p) + "%), light: " +
             String(brightness) + "%\n";
  Serial.print(s);
  sendText(s);
}

class ServerCB : public BLEServerCallbacks {
  void onConnect(BLEServer*) override { connected = true; }
  void onDisconnect(BLEServer*) override {
    connected = false;
    BLEDevice::startAdvertising();
  }
};

class RxCB : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override {
    String cmd = String(c->getValue().c_str());
    cmd.trim();
    cmd.toLowerCase();
    if (cmd.length() == 0) return;

    if (cmd == "?") {
      sendBattery();
    } else if (cmd == "on") {
      setBrightness(100);
      sendText("OK 100%\n");
    } else if (cmd == "off") {
      setBrightness(0);
      sendText("OK 0%\n");
    } else if (isDigit(cmd[0])) {
      setBrightness(cmd.toInt());
      sendText("OK " + String(brightness) + "%\n");
    } else {
      sendText("Commands: 0-100, on, off, ?\n");
    }
  }
};

void setup() {
  Serial.begin(115200);

  pwmInit();
  setBrightness(0);

  analogReadResolution(12);
  analogSetPinAttenuation(BAT_PIN, ADC_11db);

  BLEDevice::init(DEVICE_NAME);
  server = BLEDevice::createServer();
  server->setCallbacks(new ServerCB());

  BLEService* service = server->createService(SERVICE_UUID);

  txChar = service->createCharacteristic(CHAR_TX_UUID, BLECharacteristic::PROPERTY_NOTIFY);
  txChar->addDescriptor(new BLE2902());

  BLECharacteristic* rxChar = service->createCharacteristic(
      CHAR_RX_UUID,
      BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  rxChar->setCallbacks(new RxCB());

  service->start();

  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(SERVICE_UUID);
  adv->setScanResponse(true);
  BLEDevice::startAdvertising();

  Serial.println("BLE ready: " DEVICE_NAME);
}

void loop() {
  if (millis() - lastBat >= BAT_INTERVAL) {
    lastBat = millis();
    if (connected) sendBattery();
  }
  delay(10);
}
