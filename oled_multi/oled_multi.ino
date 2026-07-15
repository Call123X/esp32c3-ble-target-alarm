
#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <math.h>
#include <Preferences.h>

#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>

// ================== Hardware ==================
// ESP32-C3 SuperMini usually has an onboard LED on GPIO8.
// Change this if your board uses a different onboard LED pin.
#define BOARD_LED_PIN 8

// 0.42 inch SSD1306 72x40 OLED, same style as the reference scanner.
// If your board uses other I2C pins, change these two values.
#define OLED_SDA_PIN 5
#define OLED_SCL_PIN 6
#define OLED_I2C_ADDR 0x3C
#define OLED_X_OFFSET 28

// Many ESP32-C3 onboard LEDs are active-low.
#define BOARD_LED_ACTIVE_LOW 1

// ================== WiFi AP ==================
const char* AP_SSID = "ESP32C3-BLE";
const char* AP_PASS = "12345678";

// ================== BLE scan ==================
#define SCAN_SECONDS 3
#define SCAN_INTERVAL_MS 6000
#define MAX_DEVICES 80
#define MAX_RULES 8

// RSSI distance estimate. RSSI is not a real ruler; tune these on-site.
#define RSSI_AT_1M -59
#define PATH_LOSS_N 2.2f
#define ALERT_MIN_RSSI -98

// Passive buzzer tone.
#define BUZZER_FREQ_HZ 2300

enum OutputType {
  OUTPUT_LAMP,
  OUTPUT_PASSIVE_BUZZER
};

struct TargetOutputRule {
  String address;
  String label;
  uint8_t pin;
  OutputType type;
  bool activeLow;
  int toneHz;
  bool enabled;
  bool found;
  int rssi;
  float distanceM;
  bool state;
  unsigned long lastToggleMs;
};

TargetOutputRule targetRules[MAX_RULES];
const int TARGET_RULE_COUNT = MAX_RULES;

String normalizeAddr(String address);

WebServer server(80);
BLEScan* bleScan = nullptr;
Preferences prefs;

// ================== Tiny SSD1306 driver ==================
class TinySSD1306_72x40 {
public:
  bool begin(uint8_t addr, uint8_t sdaPin, uint8_t sclPin) {
    i2cAddr = addr;
    Wire.begin(sdaPin, sclPin, 400000);
    delay(50);

    static const uint8_t initSeq[] = {
      0xAE, 0xD5, 0x80, 0xA8, 0x27, 0xD3, 0x00, 0x40,
      0x8D, 0x14, 0x20, 0x00, 0xA1, 0xC8, 0xDA, 0x12,
      0x81, 0x7F, 0xD9, 0xF1, 0xDB, 0x40, 0xA4, 0xA6, 0xAF
    };

    for (size_t i = 0; i < sizeof(initSeq); i++) {
      command(initSeq[i]);
    }

    clear();
    send();
    return true;
  }

  void clear() {
    memset(buffer, 0, sizeof(buffer));
  }

  void pixel(int x, int y) {
    if (x < 0 || x >= 72 || y < 0 || y >= 40) return;
    buffer[x + (y / 8) * 72] |= (1 << (y & 7));
  }

  void hline(int x, int y, int w) {
    for (int i = 0; i < w; i++) pixel(x + i, y);
  }

  void text(int x, int y, String s, int scale = 1) {
    s.toUpperCase();
    int cx = x;

    for (int i = 0; i < s.length(); i++) {
      const uint8_t* g = glyph(s[i]);
      if (!g) {
        cx += 4 * scale;
        continue;
      }

      for (int row = 0; row < 7; row++) {
        for (int col = 0; col < 5; col++) {
          if (g[row] & (1 << (4 - col))) {
            for (int sy = 0; sy < scale; sy++) {
              for (int sx = 0; sx < scale; sx++) {
                pixel(cx + col * scale + sx, y + row * scale + sy);
              }
            }
          }
        }
      }

      cx += 6 * scale;
    }
  }

  int textWidth(String s, int scale = 1) {
    return s.length() * 6 * scale;
  }

  void send() {
    for (int page = 0; page < 5; page++) {
      command(0x21);
      command(OLED_X_OFFSET);
      command(OLED_X_OFFSET + 71);
      command(0x22);
      command(page);
      command(page);

      Wire.beginTransmission(i2cAddr);
      Wire.write((uint8_t)0x40);
      for (int col = 0; col < 72; col++) {
        Wire.write(buffer[page * 72 + col]);
      }
      Wire.endTransmission();
    }
  }

private:
  uint8_t i2cAddr = OLED_I2C_ADDR;
  uint8_t buffer[72 * 5];

  void command(uint8_t c) {
    Wire.beginTransmission(i2cAddr);
    Wire.write((uint8_t)0x00);
    Wire.write(c);
    Wire.endTransmission();
  }

  const uint8_t* glyph(char c) {
    switch (c) {
      case 'A': { static const uint8_t g[7] = {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}; return g; }
      case 'B': { static const uint8_t g[7] = {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}; return g; }
      case 'C': { static const uint8_t g[7] = {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}; return g; }
      case 'D': { static const uint8_t g[7] = {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E}; return g; }
      case 'E': { static const uint8_t g[7] = {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}; return g; }
      case 'F': { static const uint8_t g[7] = {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}; return g; }
      case 'G': { static const uint8_t g[7] = {0x0E,0x11,0x10,0x17,0x11,0x11,0x0E}; return g; }
      case 'H': { static const uint8_t g[7] = {0x11,0x11,0x11,0x1F,0x11,0x11,0x11}; return g; }
      case 'I': { static const uint8_t g[7] = {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}; return g; }
      case 'J': { static const uint8_t g[7] = {0x01,0x01,0x01,0x01,0x11,0x11,0x0E}; return g; }
      case 'K': { static const uint8_t g[7] = {0x11,0x12,0x14,0x18,0x14,0x12,0x11}; return g; }
      case 'L': { static const uint8_t g[7] = {0x10,0x10,0x10,0x10,0x10,0x10,0x1F}; return g; }
      case 'M': { static const uint8_t g[7] = {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}; return g; }
      case 'N': { static const uint8_t g[7] = {0x11,0x19,0x15,0x13,0x11,0x11,0x11}; return g; }
      case 'O': { static const uint8_t g[7] = {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}; return g; }
      case 'P': { static const uint8_t g[7] = {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}; return g; }
      case 'Q': { static const uint8_t g[7] = {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}; return g; }
      case 'R': { static const uint8_t g[7] = {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}; return g; }
      case 'S': { static const uint8_t g[7] = {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}; return g; }
      case 'T': { static const uint8_t g[7] = {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}; return g; }
      case 'U': { static const uint8_t g[7] = {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}; return g; }
      case 'V': { static const uint8_t g[7] = {0x11,0x11,0x11,0x11,0x11,0x0A,0x04}; return g; }
      case 'W': { static const uint8_t g[7] = {0x11,0x11,0x11,0x15,0x15,0x15,0x0A}; return g; }
      case 'X': { static const uint8_t g[7] = {0x11,0x0A,0x04,0x04,0x04,0x0A,0x11}; return g; }
      case 'Y': { static const uint8_t g[7] = {0x11,0x11,0x0A,0x04,0x04,0x04,0x04}; return g; }
      case 'Z': { static const uint8_t g[7] = {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}; return g; }
      case '0': { static const uint8_t g[7] = {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}; return g; }
      case '1': { static const uint8_t g[7] = {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}; return g; }
      case '2': { static const uint8_t g[7] = {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}; return g; }
      case '3': { static const uint8_t g[7] = {0x1E,0x01,0x01,0x0E,0x01,0x01,0x1E}; return g; }
      case '4': { static const uint8_t g[7] = {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}; return g; }
      case '5': { static const uint8_t g[7] = {0x1F,0x10,0x10,0x1E,0x01,0x01,0x1E}; return g; }
      case '6': { static const uint8_t g[7] = {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}; return g; }
      case '7': { static const uint8_t g[7] = {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}; return g; }
      case '8': { static const uint8_t g[7] = {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}; return g; }
      case '9': { static const uint8_t g[7] = {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}; return g; }
      case '-': { static const uint8_t g[7] = {0x00,0x00,0x00,0x1F,0x00,0x00,0x00}; return g; }
      case '.': { static const uint8_t g[7] = {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C}; return g; }
      case ':': { static const uint8_t g[7] = {0x00,0x0C,0x0C,0x00,0x0C,0x0C,0x00}; return g; }
      case '/': { static const uint8_t g[7] = {0x01,0x01,0x02,0x04,0x08,0x10,0x10}; return g; }
      default: return nullptr;
    }
  }
};

TinySSD1306_72x40 oled;

// ================== Data ==================
struct BLEInfo {
  String address;
  String name;
  int rssi;
  float distanceM;
};

BLEInfo devices[MAX_DEVICES];
int deviceCount = 0;

bool monitoredFound = false;
int bestMonitoredRSSI = -999;
float bestMonitoredDistance = 0.0f;
String bestMonitoredName = "";
String bestMonitoredAddr = "";

unsigned long lastScanMs = 0;
unsigned long lastBlinkMs = 0;
bool lampState = false;

// ================== GPIO ==================
void boardLedWrite(bool on) {
#if BOARD_LED_ACTIVE_LOW
  digitalWrite(BOARD_LED_PIN, on ? LOW : HIGH);
#else
  digitalWrite(BOARD_LED_PIN, on ? HIGH : LOW);
#endif
}

bool hasRuleAddress(const TargetOutputRule& rule) {
  return rule.enabled && rule.address.length() == 17;
}

void outputWrite(TargetOutputRule& rule, bool on) {
  if (rule.type == OUTPUT_PASSIVE_BUZZER) {
    if (on) {
      tone(rule.pin, rule.toneHz > 0 ? rule.toneHz : BUZZER_FREQ_HZ);
    } else {
      noTone(rule.pin);
    }
    return;
  }

  if (rule.activeLow) {
    digitalWrite(rule.pin, on ? LOW : HIGH);
  } else {
    digitalWrite(rule.pin, on ? HIGH : LOW);
  }
}

void setupConfiguredOutputs() {
  for (int i = 0; i < TARGET_RULE_COUNT; i++) {
    if (!hasRuleAddress(targetRules[i])) continue;
    pinMode(targetRules[i].pin, OUTPUT);
    outputWrite(targetRules[i], false);
  }
}

String ruleKey(int index, const char* suffix) {
  return String("r") + String(index) + "_" + suffix;
}

void clearRuleRuntime(TargetOutputRule& rule) {
  rule.found = false;
  rule.rssi = -999;
  rule.distanceM = 0.0f;
  rule.state = false;
  rule.lastToggleMs = 0;
}

void setRuleDefaults(int index) {
  targetRules[index].address = "";
  targetRules[index].label = "";
  targetRules[index].pin = 3 + index;
  targetRules[index].type = OUTPUT_LAMP;
  targetRules[index].activeLow = false;
  targetRules[index].toneHz = BUZZER_FREQ_HZ;
  targetRules[index].enabled = false;
  clearRuleRuntime(targetRules[index]);
}

void saveRule(int index) {
  if (index < 0 || index >= TARGET_RULE_COUNT) return;
  TargetOutputRule& rule = targetRules[index];
  prefs.putString(ruleKey(index, "addr").c_str(), rule.address);
  prefs.putString(ruleKey(index, "label").c_str(), rule.label);
  prefs.putUChar(ruleKey(index, "pin").c_str(), rule.pin);
  prefs.putUChar(ruleKey(index, "type").c_str(), rule.type == OUTPUT_PASSIVE_BUZZER ? 1 : 0);
  prefs.putBool(ruleKey(index, "low").c_str(), rule.activeLow);
  prefs.putUShort(ruleKey(index, "hz").c_str(), rule.toneHz);
  prefs.putBool(ruleKey(index, "en").c_str(), rule.enabled);
}

void loadRules() {
  for (int i = 0; i < TARGET_RULE_COUNT; i++) {
    setRuleDefaults(i);
    targetRules[i].address = normalizeAddr(prefs.getString(ruleKey(i, "addr").c_str(), ""));
    targetRules[i].label = prefs.getString(ruleKey(i, "label").c_str(), "");
    targetRules[i].pin = prefs.getUChar(ruleKey(i, "pin").c_str(), 3 + i);
    targetRules[i].type = prefs.getUChar(ruleKey(i, "type").c_str(), 0) == 1 ? OUTPUT_PASSIVE_BUZZER : OUTPUT_LAMP;
    targetRules[i].activeLow = prefs.getBool(ruleKey(i, "low").c_str(), false);
    targetRules[i].toneHz = prefs.getUShort(ruleKey(i, "hz").c_str(), BUZZER_FREQ_HZ);
    targetRules[i].enabled = prefs.getBool(ruleKey(i, "en").c_str(), false) && targetRules[i].address.length() == 17;
    clearRuleRuntime(targetRules[i]);
  }
}

void deleteRule(int index) {
  if (index < 0 || index >= TARGET_RULE_COUNT) return;
  outputWrite(targetRules[index], false);
  setRuleDefaults(index);
  saveRule(index);
}

void resetConfiguredTargets() {
  for (int i = 0; i < TARGET_RULE_COUNT; i++) {
    targetRules[i].found = false;
    targetRules[i].rssi = -999;
    targetRules[i].distanceM = 0.0f;
  }
}

void updateConfiguredTarget(String address, int rssi, float distanceM) {
  address = normalizeAddr(address);

  for (int i = 0; i < TARGET_RULE_COUNT; i++) {
    if (!hasRuleAddress(targetRules[i])) continue;

    String ruleAddress = normalizeAddr(targetRules[i].address);
    if (ruleAddress != address) continue;

    targetRules[i].found = true;
    if (rssi > targetRules[i].rssi) {
      targetRules[i].rssi = rssi;
      targetRules[i].distanceM = distanceM;
    }
  }
}

bool updateConfiguredOutputs(unsigned long now) {
  bool anyFound = false;

  for (int i = 0; i < TARGET_RULE_COUNT; i++) {
    TargetOutputRule& rule = targetRules[i];
    if (!hasRuleAddress(rule)) continue;

    if (!rule.found) {
      outputWrite(rule, false);
      rule.state = false;
      continue;
    }

    anyFound = true;

    if (rule.type == OUTPUT_LAMP) {
      if (now - rule.lastToggleMs >= 260) {
        rule.lastToggleMs = now;
        rule.state = !rule.state;
        outputWrite(rule, rule.state);
      }
    } else {
      int intervalMs = map(constrain(rule.rssi, -95, -45), -95, -45, 1100, 360);
      int beepMs = 140;

      if (!rule.state && now - rule.lastToggleMs >= (unsigned long)intervalMs) {
        rule.lastToggleMs = now;
        rule.state = true;
        outputWrite(rule, true);
      } else if (rule.state && now - rule.lastToggleMs >= (unsigned long)beepMs) {
        rule.lastToggleMs = now;
        rule.state = false;
        outputWrite(rule, false);
      }
    }
  }

  return anyFound;
}

void updateBestConfiguredTarget() {
  monitoredFound = false;
  bestMonitoredRSSI = -999;
  bestMonitoredDistance = 0.0f;
  bestMonitoredName = "";
  bestMonitoredAddr = "";

  for (int i = 0; i < TARGET_RULE_COUNT; i++) {
    TargetOutputRule& rule = targetRules[i];
    if (!rule.found || rule.rssi < ALERT_MIN_RSSI) continue;
    if (!monitoredFound || rule.rssi > bestMonitoredRSSI) {
      monitoredFound = true;
      bestMonitoredRSSI = rule.rssi;
      bestMonitoredDistance = rule.distanceM;
      bestMonitoredName = rule.label.length() ? rule.label : rule.address;
      bestMonitoredAddr = rule.address;
    }
  }
}

// ================== Helpers ==================
String normalizeAddr(String address) {
  address.toLowerCase();
  return address;
}

String jsonEscape(String s) {
  String out = "";

  for (int i = 0; i < s.length(); i++) {
    uint8_t c = (uint8_t)s[i];

    if (c == '\\') {
      out += "\\\\";
    } else if (c == '"') {
      out += "\\\"";
    } else if (c == '\n') {
      out += "\\n";
    } else if (c == '\r') {
      out += "\\r";
    } else if (c == '\t') {
      out += "\\t";
    } else if (c < 0x20) {
      char buf[7];
      sprintf(buf, "\\u%04X", c);
      out += buf;
    } else {
      out += (char)c;
    }
  }

  return out;
}

String bytesToString(const uint8_t* data, size_t len) {
  String out = "";
  for (size_t i = 0; i < len; i++) {
    if (data[i] == 0) continue;
    out += (char)data[i];
  }
  out.trim();
  return out;
}

String nameFromPayload(uint8_t* payload, size_t payloadLen) {
  if (payload == nullptr || payloadLen == 0) return "";

  String shortName = "";
  size_t pos = 0;

  while (pos < payloadLen) {
    uint8_t fieldLen = payload[pos];
    if (fieldLen == 0) break;

    size_t fieldEnd = pos + 1 + fieldLen;
    if (fieldEnd > payloadLen || fieldLen < 1) break;

    uint8_t type = payload[pos + 1];
    const uint8_t* value = payload + pos + 2;
    size_t valueLen = fieldLen - 1;

    if (type == 0x09 && valueLen > 0) {
      return bytesToString(value, valueLen);
    }

    if (type == 0x08 && valueLen > 0 && shortName.length() == 0) {
      shortName = bytesToString(value, valueLen);
    }

    pos = fieldEnd;
  }

  return shortName;
}

float estimateDistanceM(int rssi) {
  if (rssi >= 0 || rssi < -120) return 0.0f;
  float ratio = ((float)RSSI_AT_1M - (float)rssi) / (10.0f * PATH_LOSS_N);
  float d = pow(10.0f, ratio);
  if (d < 0.1f) d = 0.1f;
  if (d > 99.0f) d = 99.0f;
  return d;
}

String distanceText(float d) {
  if (d <= 0.0f) return "-";
  if (d >= 10.0f) return String((int)(d + 0.5f)) + "m";
  return String(d, 1) + "m";
}

int findDevice(String address) {
  address = normalizeAddr(address);
  for (int i = 0; i < deviceCount; i++) {
    if (devices[i].address == address) return i;
  }
  return -1;
}

void sortDevices() {
  for (int i = 0; i < deviceCount - 1; i++) {
    for (int j = i + 1; j < deviceCount; j++) {
      if (devices[j].rssi > devices[i].rssi) {
        BLEInfo tmp = devices[i];
        devices[i] = devices[j];
        devices[j] = tmp;
      }
    }
  }
}

// ================== OLED ==================
void drawOled() {
  oled.clear();

  int configuredCount = 0;
  for (int i = 0; i < TARGET_RULE_COUNT; i++) {
    if (targetRules[i].enabled) configuredCount++;
  }

  if (configuredCount == 0) {
    oled.text(0, 0, "BLE MON");
    oled.hline(0, 10, 72);
    oled.text(3, 18, "SELECT", 1);
    oled.text(3, 30, "ON WEB", 1);
    oled.send();
    return;
  }

  if (!monitoredFound) {
    oled.text(0, 0, "TARGET");
    oled.hline(0, 10, 72);
    oled.text(8, 18, "LOST", 2);
    oled.send();
    return;
  }

  String d = distanceText(bestMonitoredDistance);
  d.toUpperCase();
  int x = (72 - oled.textWidth(d, 2)) / 2;
  if (x < 0) x = 0;

  oled.text(0, 0, "ALERT");
  oled.text(44, 0, String(bestMonitoredRSSI));
  oled.hline(0, 10, 72);
  oled.text(x, 17, d, 2);
  oled.send();
}

// ================== BLE callbacks ==================
class ScanCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    String address = normalizeAddr(advertisedDevice.getAddress().toString().c_str());
    String name = "";

    if (advertisedDevice.haveName()) {
      name = advertisedDevice.getName();
      name.trim();
    }

    if (name.length() == 0) {
      name = nameFromPayload(advertisedDevice.getPayload(), advertisedDevice.getPayloadLength());
    }

    int rssi = advertisedDevice.getRSSI();
    int index = findDevice(address);

    if (index < 0) {
      if (deviceCount >= MAX_DEVICES) return;
      index = deviceCount++;
      devices[index].address = address;
      devices[index].name = "";
      devices[index].rssi = -999;
      devices[index].distanceM = 0.0f;
    }

    if (name.length() > 0) devices[index].name = name;
    if (rssi > devices[index].rssi) {
      devices[index].rssi = rssi;
      devices[index].distanceM = estimateDistanceM(rssi);
    }

    updateConfiguredTarget(address, devices[index].rssi, devices[index].distanceM);
  }
};

// ================== Scan ==================
void performScan() {
  Serial.println("Scanning BLE...");

  deviceCount = 0;
  monitoredFound = false;
  bestMonitoredRSSI = -999;
  bestMonitoredDistance = 0.0f;
  resetConfiguredTargets();

  bleScan->start(SCAN_SECONDS, false);
  bleScan->clearResults();
  sortDevices();
  updateBestConfiguredTarget();
  drawOled();

  Serial.print("Devices: ");
  Serial.print(deviceCount);
  Serial.print(", configured target found: ");
  Serial.println(monitoredFound ? "yes" : "no");
}

// ================== Alarm ==================
void updateAlarm() {
  unsigned long now = millis();
  bool configuredFound = updateConfiguredOutputs(now);

  if (!configuredFound) {
    boardLedWrite(false);
    lampState = false;
    return;
  }

  int blinkMs = 260;
  if (now - lastBlinkMs >= (unsigned long)blinkMs) {
    lastBlinkMs = now;
    lampState = !lampState;
    boardLedWrite(lampState);
  }
}

// ================== Web ==================
void handleRoot() {
  String html = R"rawliteral(
<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>ESP32-C3 蓝牙监测告警</title>
  <style>
    :root { color-scheme: light dark; --bg:#0f172a; --panel:#111827; --line:#334155; --text:#e5e7eb; --muted:#94a3b8; --accent:#38bdf8; --warn:#f59e0b; --ok:#22c55e; }
    * { box-sizing: border-box; }
    body { margin:0; padding:14px; font-family: Arial, "Microsoft YaHei", sans-serif; background:var(--bg); color:var(--text); }
    h1 { font-size:21px; margin:0 0 12px; }
    .panel { background:var(--panel); border:1px solid var(--line); border-radius:8px; padding:12px; margin-bottom:12px; }
    .status { display:grid; grid-template-columns:repeat(auto-fit,minmax(140px,1fr)); gap:8px; }
    .stat { border:1px solid #243244; border-radius:8px; padding:10px; min-height:64px; }
    .label { color:var(--muted); font-size:12px; margin-bottom:4px; }
    .value { font-size:18px; font-weight:700; }
    button { border:0; border-radius:7px; padding:9px 12px; color:white; background:#2563eb; font-size:14px; margin:4px 6px 4px 0; }
    button.warn { background:#dc2626; }
    button.del { background:#dc2626; }
    button.mini { padding:6px 8px; font-size:12px; }
    input, select { background:#0b1220; color:var(--text); border:1px solid var(--line); border-radius:6px; padding:6px; max-width:150px; }
    input[type="checkbox"] { width:20px; height:20px; accent-color:var(--ok); }
    .table-wrap { overflow-x:auto; }
    table { width:100%; min-width:1040px; border-collapse:collapse; font-size:13px; }
    th, td { border-bottom:1px solid var(--line); padding:8px; text-align:left; vertical-align:top; word-break:break-word; }
    th { color:#93c5fd; position:sticky; top:0; background:var(--panel); }
    .strong { color:#22c55e; font-weight:700; }
    .mid { color:#f59e0b; font-weight:700; }
    .weak { color:#fb7185; font-weight:700; }
    .on { color:#f59e0b; font-weight:700; }
    .muted { color:var(--muted); }
    .small { color:var(--muted); font-size:12px; line-height:1.55; }
  </style>
</head>
<body>
  <h1>ESP32-C3 蓝牙监测告警</h1>

  <section class="panel">
    <div class="status">
      <div class="stat"><div class="label">附近 BLE</div><div id="deviceCount" class="value">-</div></div>
      <div class="stat"><div class="label">已配置规则</div><div id="monitoredCount" class="value">-</div></div>
      <div class="stat"><div class="label">目标状态</div><div id="targetStatus" class="value">-</div></div>
      <div class="stat"><div class="label">最近目标距离</div><div id="targetDistance" class="value">-</div></div>
    </div>
    <div style="margin-top:10px">
      <button onclick="forceScan()">立即扫描</button>
    </div>
    <div class="small">连接热点：ESP32C3-BLE，密码：12345678，浏览器打开 http://192.168.4.1。距离由 RSSI 估算，会受人体遮挡、墙体、手机发射功率影响。</div>
  </section>

  <section class="panel table-wrap">
    <h3>网页配置规则</h3>
    <div class="small">在这里给蓝牙 MAC 分配 GPIO，并选择这个 GPIO 是灯还是无源蜂鸣器。保存后会写入 ESP32-C3，重启还在。</div>
    <table>
      <thead>
        <tr>
          <th>状态</th>
          <th>备注</th>
          <th>MAC</th>
          <th>GPIO</th>
          <th>类型</th>
          <th>低电平</th>
          <th>频率</th>
          <th>RSSI</th>
          <th>距离</th>
          <th>操作</th>
        </tr>
      </thead>
      <tbody id="ruleRows"><tr><td colspan="10">正在读取...</td></tr></tbody>
    </table>
  </section>

  <section class="panel table-wrap">
    <table>
      <thead>
        <tr>
          <th>信号</th>
          <th>估算距离</th>
          <th>广播名称</th>
          <th>MAC 地址</th>
          <th>操作</th>
        </tr>
      </thead>
      <tbody id="rows"><tr><td colspan="10">正在读取...</td></tr></tbody>
    </table>
  </section>

<script>
let busy = false;

const esc = (v) => {
  return String(v ?? "").replace(/[&<>"']/g, c => ({ "&":"&amp;", "<":"&lt;", ">":"&gt;", '"':"&quot;", "'":"&#39;" }[c]));
};

const enc = (v) => encodeURIComponent(String(v ?? ""));

const rssiClass = (v) => {
  if (v >= -65) return "strong";
  if (v >= -82) return "mid";
  return "weak";
};

const forceScan = async () => {
  if (busy) return;
  busy = true;
  document.getElementById("targetStatus").textContent = "扫描中";
  await fetch("/scan");
  busy = false;
  await loadData();
};

const firstEmpty = (rules) => {
  const i = rules.findIndex(r => !r.enabled);
  return i < 0 ? 0 : i;
};

const fillFromDevice = (addr, name) => {
  const rules = window.lastData.rules || [];
  const i = firstEmpty(rules);
  document.getElementById(`addr${i}`).value = addr;
  document.getElementById(`label${i}`).value = name || addr;
  document.getElementById(`en${i}`).checked = true;
  window.scrollTo(0, 0);
};

const saveRule = async (i) => {
  const q = new URLSearchParams({
    i,
    addr: document.getElementById(`addr${i}`).value.trim(),
    label: document.getElementById(`label${i}`).value.trim(),
    pin: document.getElementById(`pin${i}`).value,
    type: document.getElementById(`type${i}`).value,
    low: document.getElementById(`low${i}`).checked ? 1 : 0,
    hz: document.getElementById(`hz${i}`).value,
    en: document.getElementById(`en${i}`).checked ? 1 : 0
  });
  await fetch(`/rule?${q.toString()}`);
  await loadData();
};

const delRule = async (i) => {
  await fetch(`/delete?i=${i}`);
  await loadData();
};

const render = (data) => {
  window.lastData = data;
  document.getElementById("deviceCount").textContent = `${data.deviceCount} 个`;
  document.getElementById("monitoredCount").textContent = `${data.monitoredCount} 个`;
  document.getElementById("targetStatus").textContent = data.monitoredFound ? "已发现" : (data.monitoredCount ? "未发现" : "未配置");
  document.getElementById("targetDistance").textContent = data.monitoredFound ? data.bestDistance : "-";

  document.getElementById("ruleRows").innerHTML = (data.rules || []).map(r => `
    <tr>
      <td>${r.found ? '<span class="on">发现</span>' : '<span class="muted">未发现</span>'}<br><label><input id="en${r.index}" type="checkbox" ${r.enabled ? "checked" : ""}>启用</label></td>
      <td><input id="label${r.index}" value="${esc(r.label)}"></td>
      <td><input id="addr${r.index}" value="${esc(r.address)}" placeholder="aa:bb:cc:dd:ee:ff"></td>
      <td><input id="pin${r.index}" type="number" min="0" max="21" value="${r.pin}"></td>
      <td><select id="type${r.index}"><option value="lamp" ${r.type === "灯" ? "selected" : ""}>灯</option><option value="buzzer" ${r.type === "蜂鸣器" ? "selected" : ""}>蜂鸣器</option></select></td>
      <td><input id="low${r.index}" type="checkbox" ${r.activeLow ? "checked" : ""}></td>
      <td><input id="hz${r.index}" type="number" value="${r.toneHz}"></td>
      <td>${r.rssi}</td>
      <td>${esc(r.distance)}</td>
      <td><button class="mini" onclick="saveRule(${r.index})">保存</button><button class="mini del" onclick="delRule(${r.index})">删除</button></td>
    </tr>
  `).join("");

  const rows = document.getElementById("rows");
  if (!data.devices.length) {
    rows.innerHTML = '<tr><td colspan="10">本轮扫描没有发现 BLE 广播</td></tr>';
    return;
  }

  rows.innerHTML = data.devices.map(d => `
    <tr>
      <td class="${rssiClass(d.rssi)}">${d.rssi} dBm</td>
      <td>${esc(d.distance)}</td>
      <td>${esc(d.name || "未广播名称")}</td>
      <td>${esc(d.address)}</td>
      <td><button class="mini" onclick="fillFromDevice(decodeURIComponent('${enc(d.address)}'),decodeURIComponent('${enc(d.name || "")}'))">填到规则</button></td>
    </tr>
  `).join("");
};

const loadData = async () => {
  if (busy) return;
  try {
    const res = await fetch("/api");
    render(await res.json());
  } catch (e) {
    document.getElementById("rows").innerHTML = '<tr><td colspan="10">读取失败</td></tr>';
  }
};

loadData();
setInterval(loadData, 2500);
</script>
</body>
</html>
)rawliteral";

  server.send(200, "text/html; charset=utf-8", html);
}

void handleApi() {
  updateBestConfiguredTarget();
  int configuredCount = 0;
  for (int i = 0; i < TARGET_RULE_COUNT; i++) {
    if (targetRules[i].enabled) configuredCount++;
  }

  String json = "{";
  json += "\"deviceCount\":" + String(deviceCount) + ",";
  json += "\"monitoredCount\":" + String(configuredCount) + ",";
  json += "\"monitoredFound\":" + String(monitoredFound ? "true" : "false") + ",";
  json += "\"bestRSSI\":" + String(bestMonitoredRSSI) + ",";
  json += "\"bestDistance\":\"" + jsonEscape(distanceText(bestMonitoredDistance)) + "\",";
  json += "\"bestName\":\"" + jsonEscape(bestMonitoredName) + "\",";
  json += "\"bestAddress\":\"" + jsonEscape(bestMonitoredAddr) + "\",";
  json += "\"rules\":[";

  for (int i = 0; i < TARGET_RULE_COUNT; i++) {
    TargetOutputRule& rule = targetRules[i];
    if (i > 0) json += ",";
    json += "{";
    json += "\"index\":" + String(i) + ",";
    json += "\"enabled\":" + String(rule.enabled ? "true" : "false") + ",";
    json += "\"address\":\"" + jsonEscape(rule.address) + "\",";
    json += "\"label\":\"" + jsonEscape(rule.label) + "\",";
    json += "\"pin\":" + String(rule.pin) + ",";
    json += "\"type\":\"" + String(rule.type == OUTPUT_PASSIVE_BUZZER ? "蜂鸣器" : "灯") + "\",";
    json += "\"activeLow\":" + String(rule.activeLow ? "true" : "false") + ",";
    json += "\"toneHz\":" + String(rule.toneHz) + ",";
    json += "\"found\":" + String(rule.found ? "true" : "false") + ",";
    json += "\"rssi\":" + String(rule.rssi) + ",";
    json += "\"distance\":\"" + jsonEscape(distanceText(rule.distanceM)) + "\"";
    json += "}";
  }

  json += "],";
  json += "\"devices\":[";

  for (int i = 0; i < deviceCount; i++) {
    if (i > 0) json += ",";
    json += "{";
    json += "\"address\":\"" + jsonEscape(devices[i].address) + "\",";
    json += "\"name\":\"" + jsonEscape(devices[i].name) + "\",";
    json += "\"rssi\":" + String(devices[i].rssi) + ",";
    json += "\"distance\":\"" + jsonEscape(distanceText(devices[i].distanceM)) + "\"";
    json += "}";
  }

  json += "]}";
  server.send(200, "application/json; charset=utf-8", json);
}

void handleScan() {
  performScan();
  server.send(200, "text/plain; charset=utf-8", "OK");
}

void handleRule() {
  int index = server.arg("i").toInt();
  if (index < 0 || index >= TARGET_RULE_COUNT) {
    server.send(400, "text/plain; charset=utf-8", "bad rule index");
    return;
  }

  outputWrite(targetRules[index], false);

  targetRules[index].address = normalizeAddr(server.arg("addr"));
  targetRules[index].label = server.arg("label");
  targetRules[index].pin = (uint8_t)constrain(server.arg("pin").toInt(), 0, 21);
  targetRules[index].type = server.arg("type") == "buzzer" ? OUTPUT_PASSIVE_BUZZER : OUTPUT_LAMP;
  targetRules[index].activeLow = server.arg("low") == "1" || server.arg("low") == "true";
  targetRules[index].toneHz = constrain(server.arg("hz").toInt(), 200, 8000);
  targetRules[index].enabled = (server.arg("en") == "1" || server.arg("en") == "true") && targetRules[index].address.length() == 17;
  clearRuleRuntime(targetRules[index]);
  pinMode(targetRules[index].pin, OUTPUT);
  outputWrite(targetRules[index], false);
  saveRule(index);

  server.send(200, "text/plain; charset=utf-8", "OK");
}

void handleDelete() {
  int index = server.arg("i").toInt();
  if (index < 0 || index >= TARGET_RULE_COUNT) {
    server.send(400, "text/plain; charset=utf-8", "bad rule index");
    return;
  }

  deleteRule(index);
  server.send(200, "text/plain; charset=utf-8", "OK");
}

// ================== Arduino ==================
void setup() {
  Serial.begin(115200);
  delay(800);

  pinMode(BOARD_LED_PIN, OUTPUT);
  boardLedWrite(false);

  oled.begin(OLED_I2C_ADDR, OLED_SDA_PIN, OLED_SCL_PIN);
  oled.clear();
  oled.text(0, 0, "BLE MON");
  oled.hline(0, 10, 72);
  oled.text(8, 18, "BOOT", 2);
  oled.send();

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);

  Serial.println();
  Serial.println("ESP32-C3 BLE monitor alarm");
  Serial.print("AP SSID: ");
  Serial.println(AP_SSID);
  Serial.print("AP PASS: ");
  Serial.println(AP_PASS);
  Serial.print("Open: http://");
  Serial.println(WiFi.softAPIP());

  prefs.begin("blemon", false);
  loadRules();
  setupConfiguredOutputs();

  server.on("/", handleRoot);
  server.on("/api", handleApi);
  server.on("/scan", handleScan);
  server.on("/rule", handleRule);
  server.on("/delete", handleDelete);
  server.begin();

  BLEDevice::init("");
  bleScan = BLEDevice::getScan();
  bleScan->setAdvertisedDeviceCallbacks(new ScanCallbacks(), true);
  bleScan->setActiveScan(true);
  bleScan->setInterval(100);
  bleScan->setWindow(99);

  performScan();
  lastScanMs = millis();
}

void loop() {
  server.handleClient();
  updateAlarm();

  unsigned long now = millis();
  if (now - lastScanMs >= SCAN_INTERVAL_MS) {
    performScan();
    lastScanMs = millis();
  }
}
