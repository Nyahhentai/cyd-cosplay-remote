/*
 * ESP32-2432S028R (CYD 2.8" 240x320, XPT2046) - BLE cosplay lighting remote
 * Libs: LovyanGFX, ESP32 BLE, Preferences
 * Board: ESP32 Dev Module | Partition: Huge APP (3MB No OTA/1MB SPIFFS)
 * Hold screen on boot to recalibrate touch.
 */

#include <LovyanGFX.hpp>
#include <BLEDevice.h>
#include <Preferences.h>
#include <math.h>

struct Dev;

#define PANEL_ST7789  0     // 0 = ILI9341, 1 = ST7789
#define PANEL_INVERT  false
#define PANEL_BGR     true
#define DIM_AFTER_MS  20000
#define DIM_LEVEL     40

class LGFX : public lgfx::LGFX_Device {
#if PANEL_ST7789
  lgfx::Panel_ST7789 _panel;
#else
  lgfx::Panel_ILI9341 _panel;
#endif
  lgfx::Bus_SPI _bus;
  lgfx::Light_PWM _light;
  lgfx::Touch_XPT2046 _touch;

public:
  LGFX() {
    {
      auto cfg = _bus.config();
      cfg.spi_host = SPI2_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = 26666667;
      cfg.freq_read = 20000000;
      cfg.spi_3wire = false;
      cfg.use_lock = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = 14;
      cfg.pin_mosi = 13;
      cfg.pin_miso = 12;
      cfg.pin_dc = 2;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    {
      auto cfg = _panel.config();
      cfg.pin_cs = 15;
      cfg.pin_rst = -1;
      cfg.pin_busy = -1;
      cfg.memory_width = 240;
      cfg.memory_height = 320;
      cfg.panel_width = 240;
      cfg.panel_height = 320;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      cfg.offset_rotation = 0;
      cfg.dummy_read_pixel = 8;
      cfg.dummy_read_bits = 1;
      cfg.readable = false;
      cfg.invert = PANEL_INVERT;
      cfg.rgb_order = PANEL_BGR;
      cfg.dlen_16bit = false;
      cfg.bus_shared = false;
      _panel.config(cfg);
    }
    {
      auto cfg = _light.config();
      cfg.pin_bl = 21;
      cfg.invert = false;
      cfg.freq = 44100;
      cfg.pwm_channel = 7;
      _light.config(cfg);
      _panel.setLight(&_light);
    }
    {
      auto cfg = _touch.config();
      cfg.x_min = 300;
      cfg.x_max = 3900;
      cfg.y_min = 200;
      cfg.y_max = 3700;
      cfg.pin_int = 36;
      cfg.bus_shared = false;
      cfg.offset_rotation = 0;
      cfg.spi_host = SPI3_HOST;
      cfg.freq = 1000000;
      cfg.pin_sclk = 25;
      cfg.pin_mosi = 32;
      cfg.pin_miso = 39;
      cfg.pin_cs = 33;
      _touch.config(cfg);
      _panel.setTouch(&_touch);
    }
    setPanel(&_panel);
  }
};

LGFX tft;

uint16_t C_BG, C_PANEL, C_ACCENT, C_TEXT, C_DIM, C_GREEN, C_ORANGE, C_RED, C_BTN;

void initColors() {
  C_BG     = tft.color565(14, 18, 28);
  C_PANEL  = tft.color565(30, 37, 52);
  C_ACCENT = tft.color565(45, 108, 223);
  C_TEXT   = tft.color565(240, 244, 250);
  C_DIM    = tft.color565(130, 140, 158);
  C_GREEN  = tft.color565(60, 200, 90);
  C_ORANGE = tft.color565(240, 160, 40);
  C_RED    = tft.color565(230, 70, 70);
  C_BTN    = tft.color565(62, 72, 92);
}

enum {
  FX_STATIC = 0, FX_BREATHE, FX_HEART, FX_FLICKER,
  FX_STROBE, FX_FADE, FX_IGNITE, FX_RETRACT,
  FX_GLITCH, FX_ERROR, FX_BROWNOUT, FX_SPARK,
  FX_COUNT
};
static const char* FX_NAMES[FX_COUNT] = {
  "STATIC", "BREATHE", "HEART", "FLICKER", "STROBE", "FADE", "IGNITE", "RETRACT",
  "GLITCH", "ERROR", "BROWNOUT", "SPARK"
};
#define SPEED_MIN 1
#define SPEED_MAX 10
#define SPEED_DEF 5

#define MAX_DEV    3        // max simultaneous BLE connections
#define MAX_FOUND  5
#define DEV_TYPE_PWM  0

static BLEUUID SERVICE_UUID("6E400001-B5A3-F393-E0A9-E50E24DCCA9E"); // Nordic UART
static BLEUUID RX_UUID("6E400002-B5A3-F393-E0A9-E50E24DCCA9E");
static BLEUUID TX_UUID("6E400003-B5A3-F393-E0A9-E50E24DCCA9E");
static const char* STRIP_NAME = "LED-Dimmer";

struct Saved {
  uint8_t used;
  uint8_t type;
  uint8_t addrType;
  uint8_t addr[6];
  char name[16];
};

struct Dev {
  Saved s;
  BLEClient* client;
  BLERemoteCharacteristic* rx;
  volatile bool connected;
  volatile bool connecting;
  volatile bool seen;
  volatile int bat;
  volatile float volt;
  int ui;
  int sent;
  unsigned long lastSend;
  char rxBuf[40];
  volatile bool rxReady;
  uint8_t fx;
  uint8_t speed;
  int peak;
  int fxFrom;
  unsigned long fxStart;
  unsigned long fxTick;
  float fxv;
  uint8_t fxCnt;
};
Dev devs[MAX_DEV];

struct Found {
  uint8_t addr[6];
  uint8_t addrType;
  char name[16];
  int rssi;
};
Found found[MAX_FOUND];
volatile int foundCount = 0;
volatile bool discovering = false;

void resetRuntime(Dev& d) {
  d.ui = 0;
  d.sent = -1;
  d.bat = -1;
  d.fx = FX_STATIC;
  d.speed = SPEED_DEF;
  d.peak = 100;
  d.fxFrom = 0;
  d.fxStart = 0;
  d.fxTick = 0;
  d.fxv = 1.0f;
  d.fxCnt = 0;
}

void saveDevs() {
  Saved tmp[MAX_DEV];
  for (int i = 0; i < MAX_DEV; i++) tmp[i] = devs[i].s;
  Preferences p;
  p.begin("cyd", false);
  p.putBytes("devs", tmp, sizeof(tmp));
  p.end();
}

void loadDevs() {
  Saved tmp[MAX_DEV];
  Preferences p;
  p.begin("cyd", false);
  size_t n = p.getBytes("devs", tmp, sizeof(tmp));
  p.end();
  if (n == sizeof(tmp)) {
    for (int i = 0; i < MAX_DEV; i++) devs[i].s = tmp[i];
  }
}

void saveSpeeds() {
  uint8_t sp[MAX_DEV];
  for (int i = 0; i < MAX_DEV; i++) sp[i] = devs[i].speed;
  Preferences p;
  p.begin("cyd", false);
  p.putBytes("fxspd", sp, sizeof(sp));
  p.end();
}

void loadSpeeds() {
  uint8_t sp[MAX_DEV];
  Preferences p;
  p.begin("cyd", false);
  size_t n = p.getBytes("fxspd", sp, sizeof(sp));
  p.end();
  if (n == sizeof(sp)) {
    for (int i = 0; i < MAX_DEV; i++) {
      if (sp[i] >= SPEED_MIN && sp[i] <= SPEED_MAX) devs[i].speed = sp[i];
    }
  }
}

static void onNotify(int i, uint8_t* data, size_t len) {
  Dev& d = devs[i];
  size_t n = len < sizeof(d.rxBuf) - 1 ? len : sizeof(d.rxBuf) - 1;
  memcpy(d.rxBuf, data, n);
  d.rxBuf[n] = 0;
  d.rxReady = true;
}
static void notify0(BLERemoteCharacteristic*, uint8_t* d, size_t l, bool) { onNotify(0, d, l); }
static void notify1(BLERemoteCharacteristic*, uint8_t* d, size_t l, bool) { onNotify(1, d, l); }
static void notify2(BLERemoteCharacteristic*, uint8_t* d, size_t l, bool) { onNotify(2, d, l); }
typedef void (*NotifyFn)(BLERemoteCharacteristic*, uint8_t*, size_t, bool);
static NotifyFn notifyFns[MAX_DEV] = { notify0, notify1, notify2 };

class DevCB : public BLEClientCallbacks {
  int idx;
public:
  explicit DevCB(int i) : idx(i) {}
  void onConnect(BLEClient*) override {}
  void onDisconnect(BLEClient*) override {
    devs[idx].connected = false;
    devs[idx].rx = nullptr;
  }
};

static bool isStrip(BLEAdvertisedDevice& dev) {
  if (dev.haveServiceUUID() && dev.isAdvertisingService(SERVICE_UUID)) return true;
  if (dev.haveName() && strcmp(dev.getName().c_str(), STRIP_NAME) == 0) return true;
  return false;
}

class ScanCB : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice dev) override {
    if (!isStrip(dev)) return;
    uint8_t a[6];
    memcpy(a, dev.getAddress().getNative(), 6);
    uint8_t at = (uint8_t)dev.getAddressType();

    for (int i = 0; i < MAX_DEV; i++) {
      if (devs[i].s.used && memcmp(devs[i].s.addr, a, 6) == 0) {
        devs[i].seen = true;
        devs[i].s.addrType = at;
      }
    }

    if (discovering) {
      int n = foundCount;
      for (int k = 0; k < n; k++) {
        if (memcmp(found[k].addr, a, 6) == 0) { found[k].rssi = dev.getRSSI(); return; }
      }
      if (n < MAX_FOUND) {
        memcpy(found[n].addr, a, 6);
        found[n].addrType = at;
        found[n].rssi = dev.getRSSI();
        const char* nm = dev.haveName() ? dev.getName().c_str() : "?";
        strncpy(found[n].name, nm, sizeof(found[n].name) - 1);
        found[n].name[sizeof(found[n].name) - 1] = 0;
        foundCount = n + 1;
      }
    }
  }
};

void devSendText(int i, const char* s) {
  Dev& d = devs[i];
  if (!d.connected || !d.rx) return;
  d.rx->writeValue((uint8_t*)s, strlen(s), false);
}

void devSetBrightness(int i, int percent) {
  char b[8];
  snprintf(b, sizeof(b), "%d", percent);
  devSendText(i, b);
}

bool connectDev(int i) {
  Dev& d = devs[i];
  d.connecting = true;
  if (!d.client) {
    d.client = BLEDevice::createClient();
    d.client->setClientCallbacks(new DevCB(i));
  }
  BLEAddress a(d.s.addr);
  bool ok = d.client->connect(a, (esp_ble_addr_type_t)d.s.addrType);
  if (ok) {
    BLERemoteService* svc = d.client->getService(SERVICE_UUID);
    BLERemoteCharacteristic* rx = svc ? svc->getCharacteristic(RX_UUID) : nullptr;
    BLERemoteCharacteristic* tx = svc ? svc->getCharacteristic(TX_UUID) : nullptr;
    if (rx && tx) {
      if (tx->canNotify()) tx->registerForNotify(notifyFns[i]);
      d.rx = rx;
      d.connected = true;
      devSendText(i, "?");
    } else {
      d.client->disconnect();
      ok = false;
    }
  }
  d.connecting = false;
  return ok;
}

void bleTask(void*) {
  BLEScan* scan = BLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(new ScanCB());
  scan->setActiveScan(true);
  scan->setInterval(100);
  scan->setWindow(99);

  for (;;) {
    bool need = discovering;
    for (int i = 0; i < MAX_DEV; i++) {
      if (devs[i].s.used && !devs[i].connected) need = true;
    }
    if (!need) {
      vTaskDelay(pdMS_TO_TICKS(300));
      continue;
    }

    scan->start(4, false);
    scan->clearResults();

    bool any = false;
    for (int i = 0; i < MAX_DEV; i++) {
      if (devs[i].s.used && !devs[i].connected && devs[i].seen) {
        devs[i].seen = false;
        connectDev(i);
        any = true;
      }
    }
    vTaskDelay(pdMS_TO_TICKS((any || discovering) ? 300 : 3000));
  }
}

int batteryPercent(float v) {
  static const float tbl[][2] = {
    {4.20, 100}, {4.15, 95}, {4.11, 90}, {4.08, 85}, {4.02, 80},
    {3.98, 75},  {3.95, 70}, {3.91, 65}, {3.87, 60}, {3.85, 55},
    {3.84, 50},  {3.82, 45}, {3.80, 40}, {3.79, 35}, {3.77, 30},
    {3.75, 25},  {3.73, 20}, {3.71, 15}, {3.69, 10}, {3.61, 5},
    {3.27, 0}
  };
  const int cnt = sizeof(tbl) / sizeof(tbl[0]);
  if (v >= tbl[0][0]) return 100;
  if (v <= tbl[cnt - 1][0]) return 0;
  for (int i = 0; i < cnt - 1; i++) {
    if (v <= tbl[i][0] && v > tbl[i + 1][0]) {
      float k = (v - tbl[i + 1][0]) / (tbl[i][0] - tbl[i + 1][0]);
      return (int)(tbl[i + 1][1] + k * (tbl[i][1] - tbl[i + 1][1]) + 0.5f);
    }
  }
  return 0;
}

uint16_t batteryColor(int pct) {
  if (pct >= 30) return C_GREEN;
  if (pct >= 15) return C_ORANGE;
  return C_RED;
}

void handleRx(int i) {
  Dev& d = devs[i];
  if (!d.rxReady) return;
  d.rxReady = false;
  float v = 0; int p = 0, l = -1;
  bool ok = false;
  int n = sscanf(d.rxBuf, "Battery: %f V (%d%%), light: %d%%", &v, &p, &l);
  if (n >= 1) {
    ok = true;
    if (n < 3) l = -1;
  } else if (sscanf(d.rxBuf, "B %fV %d%% L%d", &v, &p, &l) == 3) {
    ok = true;
  }
  if (!ok) return;
  d.volt = v;
  d.bat = batteryPercent(v);
  if (d.fx == FX_STATIC && l >= 0 && millis() - d.lastSend > 1000 && l != d.ui) {
    d.ui = l;
    d.sent = l;
    if (l > 0) d.peak = l;
  }
}

static inline float gauss(float x, float c, float w) {
  float z = (x - c) / w;
  return expf(-z * z);
}

static inline int clampPct(float v) {
  int r = (int)(v + 0.5f);
  if (r < 0) r = 0;
  if (r > 100) r = 100;
  return r;
}

void setFx(int i, int fx) {
  Dev& d = devs[i];
  d.fx = fx;
  d.fxStart = millis();
  d.fxTick = 0;
  d.fxCnt = 0;
  d.fxv = (fx == FX_SPARK) ? 0.45f : 1.0f;
  if (fx == FX_GLITCH || fx == FX_ERROR || fx == FX_SPARK) d.fxTick = millis() + 600;
  if (fx == FX_RETRACT) {
    d.fxFrom = d.ui;
    if (d.ui > 0) d.peak = d.ui;
  }
}

int fxLevel(Dev& d, unsigned long now) {
  int u = d.ui;
  uint32_t t = now - d.fxStart;
  int sp = d.speed;

  switch (d.fx) {
    case FX_BREATHE: {
      uint32_t per = 7000 - sp * 600;
      float ph = (float)(t % per) / per;
      return clampPct(u * (0.5f - 0.5f * cosf(6.2831853f * ph)));
    }
    case FX_HEART: {
      uint32_t per = 2200 - sp * 180;
      float ph = (float)(t % per) / per;
      float v = fmaxf(gauss(ph, 0.12f, 0.06f), 0.7f * gauss(ph, 0.32f, 0.06f));
      return clampPct(u * (0.08f + 0.92f * v));
    }
    case FX_FLICKER: {
      uint32_t tick = 130 - sp * 10;
      if (now - d.fxTick >= tick) {
        d.fxTick = now;
        float target = random(50, 101) / 100.0f;
        if (random(0, 12) == 0) target = 0.25f;
        d.fxv += (target - d.fxv) * 0.6f;
      }
      return clampPct(u * d.fxv);
    }
    case FX_STROBE: {
      uint32_t half = 500 - sp * 40;
      return ((t / half) % 2 == 0) ? u : 0;
    }
    case FX_FADE: {
      uint32_t per = 5000 - sp * 400;
      float ph = (float)(t % per) / per;
      return clampPct(u * (1.0f - ph));
    }
    case FX_GLITCH:
    case FX_ERROR:
    case FX_SPARK: {
      float idleL = 1.0f, evL = 0.5f;
      int dmin = 40, dmax = 140, kmin = 1, kmax = 3;
      if (d.fx == FX_ERROR) { evL = 0.0f; dmin = 25; dmax = 90; kmin = 2; kmax = 5; }
      if (d.fx == FX_SPARK) { idleL = 0.45f; evL = 1.0f; dmin = 25; dmax = 80; kmin = 1; kmax = 3; }
      if (now >= d.fxTick) {
        uint32_t idle = (uint32_t)random(600, 3200) * (12 - sp) / 6;
        if (d.fxCnt > 0) {
          d.fxCnt--;
          d.fxv = (d.fxCnt & 1) ? evL : idleL;
          d.fxTick = (d.fxCnt == 0) ? now + idle : now + random(dmin, dmax + 1);
        } else {
          int k = random(kmin, kmax + 1);
          d.fxCnt = 2 * k - 1;
          d.fxv = evL;
          d.fxTick = now + random(dmin, dmax * 2 + 1);
        }
      }
      return clampPct(u * d.fxv);
    }
    case FX_BROWNOUT: {
      uint32_t per = 9000 - sp * 700;
      float ph = (float)(t % per) / per;
      float lvl = 1.0f;
      if (ph > 0.35f && ph < 0.80f) {
        float x = (ph - 0.35f) / 0.45f;
        lvl = 1.0f - 0.5f * sinf(3.1415927f * x);
        lvl += random(-8, 9) / 100.0f;
      }
      return clampPct(u * lvl);
    }
    case FX_IGNITE: {
      uint32_t dur = 2200 - sp * 180;
      int target = d.peak > 0 ? d.peak : 100;
      if (t >= dur) {
        d.ui = target;
        d.fx = FX_STATIC;
        return target;
      }
      float x = (float)t / dur;
      return clampPct(target * (x * x * (3.0f - 2.0f * x)));
    }
    case FX_RETRACT: {
      uint32_t dur = 2200 - sp * 180;
      if (t >= dur) {
        d.ui = 0;
        d.fx = FX_STATIC;
        return 0;
      }
      float x = (float)t / dur;
      return clampPct(d.fxFrom * (1.0f - x));
    }
    default:
      return u;
  }
}

enum { SCR_DEVICES = 0, SCR_CONTROL = 1, SCR_FX = 2, SCR_SCAN = 3 };
#define TAB_COUNT 4
#define TAB_W     80
int screen = SCR_DEVICES;
int sel = 0;
bool applyAll = false;
bool forgetArmed = false;
unsigned long forgetUntil = 0;
bool dragging = false;
bool dragSpeed = false;
bool wasTouch = false;
bool ignoreTouch = false;
bool dimmed = false;
unsigned long lastTouchMs = 0;
uint32_t lastSig = 0;
int rowDev[MAX_DEV];
int rowCount = 0;

const int SL_X0 = 30, SL_X1 = 290, SL_Y = 152;
const int BTN_Y = 186, BTN_H = 44, BTN_W = 68;
const int BTN_X[4] = {8, 84, 160, 236};

const int FXB_W = 72, FXB_H = 36;
const int FXB_Y[3] = {64, 104, 144};
inline int fxbX(int c) { return 8 + c * 78; }
const int SP_Y = 220;

void drawCentered(const char* s, int cx, int cy) {
  int w = tft.textWidth(s);
  int h = tft.fontHeight();
  tft.drawString(s, cx - w / 2, cy - h / 2);
}

void drawRight(const char* s, int xr, int y) {
  tft.drawString(s, xr - tft.textWidth(s), y);
}

void drawButton(int x, int y, int w, int h, const char* label, uint16_t color) {
  tft.fillRoundRect(x, y, w, h, 10, color);
  tft.setFont(&lgfx::fonts::FreeSansBold9pt7b);
  tft.setTextColor(C_TEXT, color);
  drawCentered(label, x + w / 2, y + h / 2);
}

void drawButtonSmall(int x, int y, int w, int h, const char* label, uint16_t color) {
  tft.fillRoundRect(x, y, w, h, 10, color);
  tft.setFont(&lgfx::fonts::Font2);
  tft.setTextColor(C_TEXT, color);
  drawCentered(label, x + w / 2, y + h / 2);
}

uint16_t statusColor(int i) {
  return devs[i].connected ? C_GREEN : C_ORANGE;
}

const char* statusText(int i) {
  if (devs[i].connected) return "Connected";
  if (devs[i].connecting) return "Connecting...";
  return "Searching...";
}

void drawTabs() {
  static const char* names[TAB_COUNT] = {"DEVICES", "CONTROL", "FX", "SCAN"};
  tft.fillRect(0, 0, 320, 31, C_PANEL);
  tft.setFont(&lgfx::fonts::Font2);
  for (int t = 0; t < TAB_COUNT; t++) {
    int x0 = t * TAB_W;
    tft.setTextColor(t == screen ? C_TEXT : C_DIM, C_PANEL);
    int w = tft.textWidth(names[t]);
    tft.drawString(names[t], x0 + (TAB_W - w) / 2, 7);
    if (t == screen) tft.fillRect(x0 + 8, 27, TAB_W - 16, 3, C_ACCENT);
  }
}

void drawDevices() {
  tft.fillRect(0, 31, 320, 209, C_BG);
  rowCount = 0;
  for (int i = 0; i < MAX_DEV; i++) {
    if (!devs[i].s.used) continue;
    int y = 38 + rowCount * 58;
    rowDev[rowCount++] = i;
    tft.fillRoundRect(8, y, 304, 52, 8, C_PANEL);
    if (i == sel) tft.drawRoundRect(8, y, 304, 52, 8, C_ACCENT);
    tft.fillCircle(28, y + 26, 8, statusColor(i));

    tft.setFont(&lgfx::fonts::FreeSansBold9pt7b);
    tft.setTextColor(C_TEXT, C_PANEL);
    tft.drawString(devs[i].s.name, 46, y + 6);

    tft.setFont(&lgfx::fonts::Font2);
    char sub[40];
    if (devs[i].connected) {
      if (devs[i].fx != FX_STATIC) snprintf(sub, sizeof(sub), "%s  max %d%%", FX_NAMES[devs[i].fx], devs[i].ui);
      else snprintf(sub, sizeof(sub), "Light %d%%", devs[i].ui);
    } else {
      snprintf(sub, sizeof(sub), "%s", statusText(i));
    }
    tft.setTextColor(C_DIM, C_PANEL);
    tft.drawString(sub, 46, y + 30);

    if (devs[i].connected && devs[i].bat >= 0) {
      char b[24];
      snprintf(b, sizeof(b), "%.2fV  %d%%", (float)devs[i].volt, (int)devs[i].bat);
      tft.setTextColor(batteryColor(devs[i].bat), C_PANEL);
      drawRight(b, 300, y + 8);
    }
  }
  if (rowCount == 0) {
    tft.setFont(&lgfx::fonts::FreeSansBold9pt7b);
    tft.setTextColor(C_TEXT, C_BG);
    drawCentered("No devices yet", 160, 100);
    tft.setFont(&lgfx::fonts::Font2);
    tft.setTextColor(C_DIM, C_BG);
    drawCentered("Open SCAN to add a strip", 160, 128);
  }
}

bool selValid() {
  return sel >= 0 && sel < MAX_DEV && devs[sel].s.used;
}

void drawControlHeader() {
  tft.fillRect(0, 31, 320, 40, C_BG);
  if (!selValid()) return;
  Dev& d = devs[sel];
  tft.fillCircle(20, 51, 8, statusColor(sel));
  tft.setFont(&lgfx::fonts::FreeSansBold9pt7b);
  tft.setTextColor(C_TEXT, C_BG);
  tft.drawString(d.s.name, 36, 36);
  tft.setFont(&lgfx::fonts::Font2);
  tft.setTextColor(C_DIM, C_BG);
  tft.drawString(statusText(sel), 36, 54);
  if (d.connected && d.bat >= 0) {
    char b[24];
    snprintf(b, sizeof(b), "%.2fV  %d%%", (float)d.volt, (int)d.bat);
    tft.setTextColor(batteryColor(d.bat), C_BG);
    drawRight(b, 312, 38);
  }
}

void drawValue() {
  if (!selValid()) return;
  tft.fillRect(60, 72, 200, 58, C_BG);
  tft.setFont(&lgfx::fonts::FreeSansBold24pt7b);
  tft.setTextColor(C_TEXT, C_BG);
  char b[8];
  snprintf(b, sizeof(b), "%d%%", devs[sel].ui);
  drawCentered(b, 160, 100);
}

void drawSlider() {
  if (!selValid()) return;
  tft.fillRect(5, 132, 310, 46, C_BG);
  int kx = SL_X0 + (SL_X1 - SL_X0) * devs[sel].ui / 100;
  tft.fillRoundRect(SL_X0, SL_Y - 5, SL_X1 - SL_X0, 10, 5, C_BTN);
  if (kx > SL_X0) tft.fillRoundRect(SL_X0, SL_Y - 5, kx - SL_X0, 10, 5, C_ACCENT);
  tft.fillCircle(kx, SL_Y, 14, C_TEXT);
}

void drawControlButtons() {
  drawButton(BTN_X[0], BTN_Y, BTN_W, BTN_H, "OFF", C_BTN);
  drawButton(BTN_X[1], BTN_Y, BTN_W, BTN_H, "ON", C_ACCENT);
  drawButton(BTN_X[2], BTN_Y, BTN_W, BTN_H, "ALL", applyAll ? C_ACCENT : C_BTN);
  drawButton(BTN_X[3], BTN_Y, BTN_W, BTN_H, forgetArmed ? "SURE?" : "DEL", forgetArmed ? C_RED : C_BTN);
}

void drawControl() {
  tft.fillRect(0, 31, 320, 209, C_BG);
  if (!selValid()) {
    tft.setFont(&lgfx::fonts::Font2);
    tft.setTextColor(C_DIM, C_BG);
    drawCentered("No device selected", 160, 100);
    drawCentered("Pick one in DEVICES", 160, 124);
    return;
  }
  drawControlHeader();
  drawValue();
  drawSlider();
  drawControlButtons();
}

void drawSpeed() {
  if (!selValid()) return;
  tft.fillRect(0, 184, 320, 56, C_BG);
  tft.setFont(&lgfx::fonts::Font2);
  tft.setTextColor(C_DIM, C_BG);
  char b[24];
  snprintf(b, sizeof(b), "Speed  %d / %d", devs[sel].speed, SPEED_MAX);
  drawCentered(b, 160, 194);
  int kx = SL_X0 + (SL_X1 - SL_X0) * (devs[sel].speed - SPEED_MIN) / (SPEED_MAX - SPEED_MIN);
  tft.fillRoundRect(SL_X0, SP_Y - 5, SL_X1 - SL_X0, 10, 5, C_BTN);
  if (kx > SL_X0) tft.fillRoundRect(SL_X0, SP_Y - 5, kx - SL_X0, 10, 5, C_ACCENT);
  tft.fillCircle(kx, SP_Y, 14, C_TEXT);
}

void drawFx() {
  tft.fillRect(0, 31, 320, 209, C_BG);
  if (!selValid()) {
    tft.setFont(&lgfx::fonts::Font2);
    tft.setTextColor(C_DIM, C_BG);
    drawCentered("No device selected", 160, 100);
    drawCentered("Pick one in DEVICES", 160, 124);
    return;
  }
  Dev& d = devs[sel];
  tft.fillCircle(20, 47, 8, statusColor(sel));
  tft.setFont(&lgfx::fonts::FreeSansBold9pt7b);
  tft.setTextColor(C_TEXT, C_BG);
  tft.drawString(d.s.name, 36, 38);
  tft.fillRoundRect(262, 37, 50, 22, 6, applyAll ? C_ACCENT : C_BTN);
  tft.setFont(&lgfx::fonts::Font2);
  tft.setTextColor(C_TEXT, applyAll ? C_ACCENT : C_BTN);
  drawCentered("ALL", 287, 48);
  for (int k = 0; k < FX_COUNT; k++) {
    int r = k / 4, c = k % 4;
    drawButtonSmall(fxbX(c), FXB_Y[r], FXB_W, FXB_H, FX_NAMES[k], (d.fx == k) ? C_ACCENT : C_BTN);
  }
  drawSpeed();
}

bool foundIsSaved(int k) {
  for (int i = 0; i < MAX_DEV; i++) {
    if (devs[i].s.used && memcmp(devs[i].s.addr, found[k].addr, 6) == 0) return true;
  }
  return false;
}

void drawScan() {
  tft.fillRect(0, 31, 320, 209, C_BG);
  tft.setFont(&lgfx::fonts::Font2);
  tft.setTextColor(C_DIM, C_BG);
  int n = foundCount;
  if (n == 0) tft.drawString("Scanning... turn the strip on", 10, 38);
  else tft.drawString("Tap a strip to add it", 10, 38);

  for (int k = 0; k < n; k++) {
    int y = 62 + k * 34;
    tft.fillRoundRect(8, y, 304, 30, 6, C_PANEL);
    char b[40];
    snprintf(b, sizeof(b), "%s  ..%02X:%02X", found[k].name, found[k].addr[4], found[k].addr[5]);
    tft.setFont(&lgfx::fonts::Font2);
    tft.setTextColor(C_TEXT, C_PANEL);
    tft.drawString(b, 18, y + 7);
    bool saved = foundIsSaved(k);
    tft.setTextColor(saved ? C_DIM : C_ACCENT, C_PANEL);
    drawRight(saved ? "added" : "+ ADD", 300, y + 7);
  }
}

uint32_t uiSignature() {
  uint32_t h = screen * 131 + sel;
  for (int i = 0; i < MAX_DEV; i++) {
    Dev& d = devs[i];
    h = h * 31 + d.s.used + (d.connected ? 2 : 0) + (d.connecting ? 4 : 0) + (uint32_t)(d.bat + 1) * 8;
    h = h * 31 + d.ui;
    h = h * 31 + d.fx;
  }
  if (screen == SCR_SCAN) h = h * 31 + foundCount + (discovering ? 1000 : 0);
  return h;
}

void drawAll() {
  drawTabs();
  if (screen == SCR_DEVICES) drawDevices();
  else if (screen == SCR_CONTROL) drawControl();
  else if (screen == SCR_FX) drawFx();
  else drawScan();
  lastSig = uiSignature();
}

void switchScreen(int s) {
  screen = s;
  forgetArmed = false;
  dragging = false;
  dragSpeed = false;
  discovering = (s == SCR_SCAN);
  if (s == SCR_SCAN) foundCount = 0;
  drawAll();
}

void pumpSend(int i, bool force) {
  Dev& d = devs[i];
  if (!d.s.used || !d.connected) return;
  unsigned long now = millis();
  int out = fxLevel(d, now);
  if (out == d.sent) return;
  unsigned long gap = (d.fx != FX_STATIC) ? 50 : 80;
  if (!force && now - d.lastSend < gap) return;
  devSetBrightness(i, out);
  d.sent = out;
  d.lastSend = now;
}

void pumpAll(bool force) {
  for (int i = 0; i < MAX_DEV; i++) pumpSend(i, force);
}

bool isTarget(int i) {
  if (!devs[i].s.used) return false;
  return i == sel || (applyAll && devs[i].connected);
}

void setFxTargets(int fx) {
  for (int i = 0; i < MAX_DEV; i++) {
    if (isTarget(i)) setFx(i, fx);
  }
}

void setSpeedTargets(int v) {
  v = constrain(v, SPEED_MIN, SPEED_MAX);
  bool changed = false;
  for (int i = 0; i < MAX_DEV; i++) {
    if (isTarget(i) && devs[i].speed != v) { devs[i].speed = v; changed = true; }
  }
  if (changed && screen == SCR_FX) drawSpeed();
}

void setLight(int v) {
  v = constrain(v, 0, 100);
  bool changed = false;
  for (int i = 0; i < MAX_DEV; i++) {
    if (!isTarget(i)) continue;
    if (devs[i].ui != v) { devs[i].ui = v; changed = true; }
    if (v > 0) devs[i].peak = v;
  }
  if (changed && screen == SCR_CONTROL) {
    drawValue();
    drawSlider();
  }
}

void forgetDev(int i) {
  Dev& d = devs[i];
  if (d.client && d.client->isConnected()) d.client->disconnect();
  d.connected = false;
  d.rx = nullptr;
  memset(&d.s, 0, sizeof(Saved));
  resetRuntime(d);
  saveDevs();
  saveSpeeds();
}

void addDevice(int k) {
  int slot = -1;
  for (int i = 0; i < MAX_DEV; i++) if (!devs[i].s.used) { slot = i; break; }
  if (slot < 0) return;
  Dev& d = devs[slot];
  memset(&d.s, 0, sizeof(Saved));
  d.s.used = 1;
  d.s.type = DEV_TYPE_PWM;
  d.s.addrType = found[k].addrType;
  memcpy(d.s.addr, found[k].addr, 6);
  snprintf(d.s.name, sizeof(d.s.name), "Strip %d", slot + 1);
  resetRuntime(d);
  d.connected = false;
  saveDevs();
  sel = slot;
  switchScreen(SCR_CONTROL);
}

void onTap(int x, int y) {
  if (y < 31) {
    int t = x / TAB_W;
    if (t > TAB_COUNT - 1) t = TAB_COUNT - 1;
    if (t != screen) switchScreen(t);
    return;
  }

  if (screen == SCR_DEVICES) {
    int r = (y - 38) / 58;
    if (y >= 38 && r >= 0 && r < rowCount && (y - 38) % 58 < 52) {
      sel = rowDev[r];
      switchScreen(SCR_CONTROL);
    }
  } else if (screen == SCR_CONTROL) {
    if (!selValid()) return;
    if (y >= 132 && y <= 178 && x >= 5 && x <= 315) {
      dragging = true;
    } else if (y >= BTN_Y && y <= BTN_Y + BTN_H) {
      for (int b = 0; b < 4; b++) {
        if (x < BTN_X[b] || x > BTN_X[b] + BTN_W) continue;
        if (b == 0) { setFxTargets(FX_STATIC); setLight(0);   pumpAll(true); }
        if (b == 1) { setFxTargets(FX_STATIC); setLight(100); pumpAll(true); }
        if (b == 2) { applyAll = !applyAll; drawControlButtons(); }
        if (b == 3) {
          if (!forgetArmed) {
            forgetArmed = true;
            forgetUntil = millis() + 3000;
            drawControlButtons();
          } else {
            forgetArmed = false;
            forgetDev(sel);
            sel = 0;
            for (int i = 0; i < MAX_DEV; i++) if (devs[i].s.used) { sel = i; break; }
            switchScreen(SCR_DEVICES);
          }
        }
      }
    }
  } else if (screen == SCR_FX) {
    if (y < 64 && x >= 258) {
      applyAll = !applyAll;
      drawFx();
      return;
    }
    if (!selValid()) return;
    for (int k = 0; k < FX_COUNT; k++) {
      int r = k / 4, c = k % 4;
      if (x >= fxbX(c) && x <= fxbX(c) + FXB_W && y >= FXB_Y[r] && y <= FXB_Y[r] + FXB_H) {
        setFxTargets(k);
        drawFx();
        return;
      }
    }
    if (y >= 204 && y <= 238 && x >= 5 && x <= 315) {
      dragSpeed = true;
    }
  } else {
    int r = (y - 62) / 34;
    if (y >= 62 && r >= 0 && r < foundCount && (y - 62) % 34 < 30) {
      if (!foundIsSaved(r)) addDevice(r);
    }
  }
}

void handleTouch() {
  int32_t tx, ty;
  bool touching = tft.getTouch(&tx, &ty);
  if (touching) {
    lastTouchMs = millis();
    if (dimmed) {
      dimmed = false;
      tft.setBrightness(255);
      ignoreTouch = true;
    }
  } else {
    ignoreTouch = false;
  }

  if (touching && !ignoreTouch) {
    if (!wasTouch) onTap(tx, ty);
    if (dragging) setLight(map(tx, SL_X0, SL_X1, 0, 100));
    if (dragSpeed) setSpeedTargets(map(tx, SL_X0, SL_X1, SPEED_MIN, SPEED_MAX));
  } else if (wasTouch && (dragging || dragSpeed)) {
    if (dragging) pumpAll(true);
    if (dragSpeed) saveSpeeds();
    dragging = false;
    dragSpeed = false;
  }
  wasTouch = touching;
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("=== SETUP START ===");
  tft.init();
  Serial.println("DISPLAY OK");
  tft.setRotation(1);
  tft.setBrightness(255);
  tft.fillScreen(TFT_BLACK);
  initColors();

  Preferences prefs;
  prefs.begin("cyd", false);

  uint16_t cal[8];
  bool have = prefs.getBytes("cal", cal, sizeof(cal)) == sizeof(cal);

  if (have) {
    tft.setFont(&lgfx::fonts::Font2);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString("Hold screen to recalibrate", 60, 110);
    delay(1500);

    int32_t rx, ry;
    if (tft.getTouchRaw(&rx, &ry)) {
      have = false;
    }

    tft.fillScreen(TFT_BLACK);
  }

  if (!have) {
    tft.calibrateTouch(cal, TFT_WHITE, TFT_BLACK, 30);
    prefs.putBytes("cal", cal, sizeof(cal));
  }

  tft.setTouchCalibrate(cal);
  prefs.end();
  Serial.println("TOUCH CAL OK");

  for (int i = 0; i < MAX_DEV; i++) resetRuntime(devs[i]);
  loadDevs();
  loadSpeeds();
  Serial.println("PREFS OK");
  bool anyDev = false;

  for (int i = 0; i < MAX_DEV; i++) {
    if (devs[i].s.used) {
      if (!anyDev) sel = i;
      anyDev = true;
    }
  }

  BLEDevice::init("CYD-Remote");
  BLEDevice::setMTU(517);
  xTaskCreatePinnedToCore(
    bleTask,
    "ble",
    12288,
    nullptr,
    1,
    nullptr,
    0
  );
  Serial.println("BLE TASK OK");
  lastTouchMs = millis();

  switchScreen(anyDev ? SCR_DEVICES : SCR_SCAN);
}

void loop() {
  handleTouch();
  pumpAll(false);

  for (int i = 0; i < MAX_DEV; i++) {
    bool had = devs[i].rxReady;
    handleRx(i);

    if (had && screen == SCR_CONTROL && i == sel && !dragging) {
      drawValue();
      drawSlider();
    }
  }

  if (forgetArmed && millis() > forgetUntil) {
    forgetArmed = false;

    if (screen == SCR_CONTROL) {
      drawControlButtons();
    }
  }

  if (!dimmed && millis() - lastTouchMs > DIM_AFTER_MS) {
    dimmed = true;
    tft.setBrightness(DIM_LEVEL);
  }

  uint32_t sig = uiSignature();

  if (sig != lastSig) {
    lastSig = sig;

    if (screen == SCR_DEVICES) {
      drawDevices();
    } else if (screen == SCR_SCAN) {
      drawScan();
    } else if (screen == SCR_FX) {
      if (!dragSpeed) drawFx();
    } else if (!dragging) {
      drawControlHeader();
      drawValue();
      drawSlider();
    }
  }

  delay(10);
}
