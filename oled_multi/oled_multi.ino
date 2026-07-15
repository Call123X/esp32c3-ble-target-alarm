
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
// User wiring:
//   GPIO1 = passive buzzer
//   GPIO2 = lamp / LED module
#define BUZZER_PIN 1
#define LAMP_PIN 2

// ESP32-C3 SuperMini usually has an onboard LED on GPIO8.
// Change this if your board uses a different onboard LED pin.
#define BOARD_LED_PIN 8

// 0.42 inch SSD1306 72x40 OLED, same style as the reference scanner.
// If your board uses other I2C pins, change these two values.
#define OLED_SDA_PIN 5
#define OLED_SCL_PIN 6
#define OLED_I2C_ADDR 0x3C
#define OLED_X_OFFSET 28

// Set to 1 if your lamp module is active-low.
#define LAMP_ACTIVE_LOW 0

// Many ESP32-C3 onboard LEDs are active-low.
#define BOARD_LED_ACTIVE_LOW 1

// ================== WiFi AP ==================
const char* AP_SSID = "ESP32C3-BLE";
const char* AP_PASS = "12345678";

// ================== BLE scan ==================
#define SCAN_SECONDS 3
#define SCAN_INTERVAL_MS 6000
#define MAX_DEVICES 80
#define MAX_MONITORED 16

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
  const char* address;
  const char* label;
  uint8_t pin;
  OutputType type;
  bool activeLow;
  int toneHz;
  bool found;
  int rssi;
  float distanceM;
  bool state;
  unsigned long lastToggleMs;
};

// ================== Multi-output rules ==================
// Fill in the BLE MAC addresses you want to watch.
// Each target can use a different GPIO and can be either a lamp/LED or a passive buzzer.
TargetOutputRule targetRules[] = {
  // address              label        GPIO  type                    activeLow  toneHz  runtime fields...
  {"aa:bb:cc:dd:ee:01", "target-1",    3,   OUTPUT_LAMP,            false,     0,      false, -999, 0.0f, false, 0},
  {"aa:bb:cc:dd:ee:02", "target-2",    4,   OUTPUT_PASSIVE_BUZZER,  false,     2300,   false, -999, 0.0f, false, 0},
  {"aa:bb:cc:dd:ee:03", "target-3",    7,   OUTPUT_LAMP,            false,     0,      false, -999, 0.0f, false, 0},
};

const int TARGET_RULE_COUNT = sizeof(targetRules) / sizeof(targetRules[0]);

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
  String brand;
  String deviceType;
  String company;
  int companyId;
  String serviceUUID;
  String manufacturerHex;
  bool monitored;
};

BLEInfo devices[MAX_DEVICES];
int deviceCount = 0;

String monitoredAddresses[MAX_MONITORED];
int monitoredCount = 0;

bool monitoredFound = false;
int bestMonitoredRSSI = -999;
float bestMonitoredDistance = 0.0f;
String bestMonitoredName = "";
String bestMonitoredAddr = "";

unsigned long lastScanMs = 0;
unsigned long lastBlinkMs = 0;
unsigned long lastBeepMs = 0;
bool lampState = false;
bool beepState = false;

// ================== GPIO ==================
void lampWrite(bool on) {
#if LAMP_ACTIVE_LOW
  digitalWrite(LAMP_PIN, on ? LOW : HIGH);
#else
  digitalWrite(LAMP_PIN, on ? HIGH : LOW);
#endif
}

void boardLedWrite(bool on) {
#if BOARD_LED_ACTIVE_LOW
  digitalWrite(BOARD_LED_PIN, on ? LOW : HIGH);
#else
  digitalWrite(BOARD_LED_PIN, on ? HIGH : LOW);
#endif
}

void buzzerWrite(bool on) {
  if (on) {
    tone(BUZZER_PIN, BUZZER_FREQ_HZ);
  } else {
    noTone(BUZZER_PIN);
  }
}

bool hasRuleAddress(const TargetOutputRule& rule) {
  return rule.address != nullptr && strlen(rule.address) == 17;
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

    String ruleAddress = normalizeAddr(String(targetRules[i].address));
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

template <typename T>
String hexManufacturer(const T& data) {
  String out = "";
  for (size_t i = 0; i < data.length(); i++) {
    char buf[4];
    sprintf(buf, "%02X ", (uint8_t)data[i]);
    out += buf;
  }
  out.trim();
  return out;
}

template <typename T>
int manufacturerCompanyId(const T& data) {
  if (data.length() < 2) return -1;
  return ((uint8_t)data[1] << 8) | (uint8_t)data[0];
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

bool containsText(String text, const char* needle) {
  text.toLowerCase();
  return text.indexOf(needle) >= 0;
}

String companyNameFromId(int id) {
  switch (id) {
    case 0x004C: return "Apple, Inc.";
    case 0x038F: return "Xiaomi Inc.";
    case 0x027D: return "HUAWEI Technologies";
    case 0x0075: return "Samsung Electronics";
    case 0x00E0: return "Google";
    case 0x0006: return "Microsoft";
    default: return id >= 0 ? String("Company ID 0x") + String(id, HEX) : "";
  }
}

String brandFromSignals(String name, int companyId, String manufacturerHex) {
  String text = name + " " + manufacturerHex;
  text.toLowerCase();

  if (companyId == 0x004C || containsText(text, "iphone") || containsText(text, "ipad") ||
      containsText(text, "airpods") || containsText(text, "apple") || containsText(text, "watch")) {
    return "Apple";
  }

  if (companyId == 0x038F || containsText(text, "xiaomi") || containsText(text, "redmi") ||
      containsText(text, "mijia") || containsText(text, "mi band") || containsText(text, "miband") ||
      containsText(text, "mi smart") || containsText(text, "yeelight")) {
    return "Xiaomi / Redmi / Mijia";
  }

  if (companyId == 0x027D || containsText(text, "huawei") || containsText(text, "honor") ||
      containsText(text, "freebuds")) {
    return "Huawei / Honor";
  }

  if (containsText(text, "vivo") || containsText(text, "iqoo")) {
    return "vivo / iQOO";
  }

  if (containsText(text, "oppo") || containsText(text, "oneplus") || containsText(text, "realme") ||
      containsText(text, "enco")) {
    return "OPPO / OnePlus / realme";
  }

  if (companyId == 0x0075 || containsText(text, "samsung") || containsText(text, "galaxy")) {
    return "Samsung";
  }

  if (companyId == 0x00E0 || containsText(text, "pixel") || containsText(text, "google")) {
    return "Google";
  }

  return "Unknown";
}

template <typename T>
String applePacketType(const T& manufacturerData) {
  if (manufacturerData.length() < 3) return "Apple BLE device";
  uint8_t t = (uint8_t)manufacturerData[2];

  switch (t) {
    case 0x02: return "Apple iBeacon";
    case 0x07: return "Apple Nearby";
    case 0x10: return "Apple Nearby / Continuity";
    case 0x12: return "Apple Handoff / Continuity";
    case 0x19: return "Apple Nearby Action";
    default: return String("Apple BLE type 0x") + String(t, HEX);
  }
}

String deviceTypeFromSignals(String name, String brand, int companyId, String appleType, String serviceUUID) {
  String text = name + " " + serviceUUID;
  text.toLowerCase();

  if (companyId == 0x004C) return appleType.length() ? appleType : "Apple BLE device";
  if (containsText(text, "airpods") || containsText(text, "buds") || containsText(text, "earbud") ||
      containsText(text, "headset") || containsText(text, "headphone") || containsText(text, "freebuds") ||
      containsText(text, "enco")) {
    return "Earbuds / headset";
  }
  if (containsText(text, "watch")) return "Smart watch";
  if (containsText(text, "band") || containsText(text, "bracelet")) return "Smart band";
  if (containsText(text, "phone") || containsText(text, "iphone") || containsText(text, "pixel")) return "Phone";
  if (containsText(text, "mijia") || containsText(text, "mi smart") || containsText(text, "sensor")) return "Smart home / sensor";
  if (containsText(text, "keyboard") || containsText(text, "mouse")) return "Input device";
  if (brand != "Unknown") return brand + " BLE device";
  return "BLE device";
}

int findDevice(String address) {
  address = normalizeAddr(address);
  for (int i = 0; i < deviceCount; i++) {
    if (devices[i].address == address) return i;
  }
  return -1;
}

bool isMonitored(String address) {
  address = normalizeAddr(address);
  for (int i = 0; i < monitoredCount; i++) {
    if (monitoredAddresses[i] == address) return true;
  }
  return false;
}

bool addMonitored(String address) {
  address = normalizeAddr(address);
  if (address.length() == 0 || isMonitored(address)) return true;
  if (monitoredCount >= MAX_MONITORED) return false;

  monitoredAddresses[monitoredCount++] = address;
  return true;
}

void removeMonitored(String address) {
  address = normalizeAddr(address);
  for (int i = 0; i < monitoredCount; i++) {
    if (monitoredAddresses[i] == address) {
      for (int j = i; j < monitoredCount - 1; j++) {
        monitoredAddresses[j] = monitoredAddresses[j + 1];
      }
      monitoredCount--;
      return;
    }
  }
}

void saveMonitors() {
  String list = "";

  for (int i = 0; i < monitoredCount; i++) {
    if (i > 0) list += ",";
    list += monitoredAddresses[i];
  }

  prefs.putString("targets", list);
}

void loadMonitors() {
  monitoredCount = 0;
  String list = prefs.getString("targets", "");
  int start = 0;

  while (start < list.length() && monitoredCount < MAX_MONITORED) {
    int comma = list.indexOf(',', start);
    String addr = comma >= 0 ? list.substring(start, comma) : list.substring(start);
    addr.trim();
    addr = normalizeAddr(addr);

    if (addr.length() > 0 && !isMonitored(addr)) {
      monitoredAddresses[monitoredCount++] = addr;
    }

    if (comma < 0) break;
    start = comma + 1;
  }

  Serial.print("Loaded monitored targets: ");
  Serial.println(monitoredCount);
}

void updateBestMonitored() {
  monitoredFound = false;
  bestMonitoredRSSI = -999;
  bestMonitoredDistance = 0.0f;
  bestMonitoredName = "";
  bestMonitoredAddr = "";

  for (int i = 0; i < deviceCount; i++) {
    if (devices[i].monitored && devices[i].rssi >= ALERT_MIN_RSSI && devices[i].rssi > bestMonitoredRSSI) {
      monitoredFound = true;
      bestMonitoredRSSI = devices[i].rssi;
      bestMonitoredDistance = devices[i].distanceM;
      bestMonitoredName = devices[i].name.length() ? devices[i].name : devices[i].brand;
      bestMonitoredAddr = devices[i].address;
    }
  }
}

void sortDevices() {
  for (int i = 0; i < deviceCount - 1; i++) {
    for (int j = i + 1; j < deviceCount; j++) {
      bool swapNeeded = false;

      if (devices[j].monitored && !devices[i].monitored) {
        swapNeeded = true;
      } else if (devices[j].monitored == devices[i].monitored && devices[j].rssi > devices[i].rssi) {
        swapNeeded = true;
      }

      if (swapNeeded) {
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

  if (monitoredCount == 0) {
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
    String serviceUUID = "";
    String manufacturerHex = "";
    String appleType = "";
    int companyId = -1;

    if (advertisedDevice.haveName()) {
      name = advertisedDevice.getName();
      name.trim();
    }

    if (name.length() == 0) {
      name = nameFromPayload(advertisedDevice.getPayload(), advertisedDevice.getPayloadLength());
    }

    if (advertisedDevice.haveServiceUUID()) {
      serviceUUID = advertisedDevice.getServiceUUID().toString().c_str();
    }

    if (advertisedDevice.haveManufacturerData()) {
      auto manufacturerData = advertisedDevice.getManufacturerData();
      manufacturerHex = hexManufacturer(manufacturerData);
      companyId = manufacturerCompanyId(manufacturerData);
      if (companyId == 0x004C) {
        appleType = applePacketType(manufacturerData);
      }
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
      devices[index].brand = "Unknown";
      devices[index].deviceType = "BLE device";
      devices[index].company = "";
      devices[index].companyId = -1;
      devices[index].serviceUUID = "";
      devices[index].manufacturerHex = "";
      devices[index].monitored = false;
    }

    if (name.length() > 0) devices[index].name = name;
    if (serviceUUID.length() > 0) devices[index].serviceUUID = serviceUUID;
    if (manufacturerHex.length() > 0) devices[index].manufacturerHex = manufacturerHex;
    if (companyId >= 0) {
      devices[index].companyId = companyId;
      devices[index].company = companyNameFromId(companyId);
    }

    if (rssi > devices[index].rssi) {
      devices[index].rssi = rssi;
      devices[index].distanceM = estimateDistanceM(rssi);
    }

    String brand = brandFromSignals(devices[index].name, devices[index].companyId, devices[index].manufacturerHex);
    devices[index].brand = brand;
    devices[index].deviceType = deviceTypeFromSignals(devices[index].name, brand, devices[index].companyId, appleType, devices[index].serviceUUID);
    devices[index].monitored = isMonitored(address);

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
  updateBestMonitored();
  drawOled();

  Serial.print("Devices: ");
  Serial.print(deviceCount);
  Serial.print(", monitored selected: ");
  Serial.print(monitoredCount);
  Serial.print(", monitored found: ");
  Serial.println(monitoredFound ? "yes" : "no");
}

// ================== Alarm ==================
void updateAlarm() {
  unsigned long now = millis();
  bool configuredFound = updateConfiguredOutputs(now);
  bool anyFound = monitoredFound || configuredFound;

  if (!anyFound) {
    lampWrite(false);
    boardLedWrite(false);
    buzzerWrite(false);
    lampState = false;
    beepState = false;
    return;
  }

  int blinkMs = 260;
  if (now - lastBlinkMs >= (unsigned long)blinkMs) {
    lastBlinkMs = now;
    lampState = !lampState;
    boardLedWrite(lampState);
    if (monitoredFound) {
      lampWrite(lampState);
    }
  }

  if (!monitoredFound) {
    lampWrite(false);
    buzzerWrite(false);
    beepState = false;
    return;
  }

  int intervalMs = map(constrain(bestMonitoredRSSI, -95, -45), -95, -45, 1100, 360);
  int beepMs = 140;

  if (!beepState && now - lastBeepMs >= (unsigned long)intervalMs) {
    beepState = true;
    lastBeepMs = now;
    buzzerWrite(true);
  }

  if (beepState && now - lastBeepMs >= (unsigned long)beepMs) {
    beepState = false;
    lastBeepMs = now;
    buzzerWrite(false);
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
    input[type="checkbox"] { width:20px; height:20px; accent-color:var(--ok); }
    .table-wrap { overflow-x:auto; }
    table { width:100%; min-width:1040px; border-collapse:collapse; font-size:13px; }
    th, td { border-bottom:1px solid var(--line); padding:8px; text-align:left; vertical-align:top; word-break:break-word; }
    th { color:#93c5fd; position:sticky; top:0; background:var(--panel); }
    tr.monitored { background:rgba(245,158,11,.13); }
    .badge { display:inline-block; border-radius:999px; padding:3px 7px; font-size:12px; font-weight:700; white-space:nowrap; }
    .apple { background:rgba(148,163,184,.22); color:#f8fafc; }
    .xiaomi { background:rgba(249,115,22,.2); color:#fdba74; }
    .huawei { background:rgba(239,68,68,.2); color:#fca5a5; }
    .oppo { background:rgba(34,197,94,.2); color:#86efac; }
    .vivo { background:rgba(59,130,246,.2); color:#93c5fd; }
    .unknown { background:rgba(148,163,184,.16); color:#cbd5e1; }
    .strong { color:#22c55e; font-weight:700; }
    .mid { color:#f59e0b; font-weight:700; }
    .weak { color:#fb7185; font-weight:700; }
    .muted { color:var(--muted); }
    .small { color:var(--muted); font-size:12px; line-height:1.55; }
  </style>
</head>
<body>
  <h1>ESP32-C3 蓝牙监测告警</h1>

  <section class="panel">
    <div class="status">
      <div class="stat"><div class="label">附近 BLE</div><div id="deviceCount" class="value">-</div></div>
      <div class="stat"><div class="label">已勾选监测</div><div id="monitoredCount" class="value">-</div></div>
      <div class="stat"><div class="label">目标状态</div><div id="targetStatus" class="value">-</div></div>
      <div class="stat"><div class="label">最近目标距离</div><div id="targetDistance" class="value">-</div></div>
    </div>
    <div style="margin-top:10px">
      <button onclick="forceScan()">立即扫描</button>
      <button class="warn" onclick="clearMonitors()">清空监测</button>
    </div>
    <div class="small">连接热点：ESP32C3-BLE，密码：12345678，浏览器打开 http://192.168.4.1。距离由 RSSI 估算，会受人体遮挡、墙体、手机发射功率影响。</div>
  </section>

  <section class="panel table-wrap">
    <table>
      <thead>
        <tr>
          <th>监测</th>
          <th>信号</th>
          <th>估算距离</th>
          <th>品牌</th>
          <th>设备类型</th>
          <th>广播名称</th>
          <th>MAC 地址</th>
          <th>厂商</th>
          <th>Service UUID</th>
          <th>厂商数据</th>
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

const rssiClass = (v) => {
  if (v >= -65) return "strong";
  if (v >= -82) return "mid";
  return "weak";
};

const brandClass = (brand) => {
  const b = String(brand || "").toLowerCase();
  if (b.includes("apple")) return "apple";
  if (b.includes("xiaomi") || b.includes("redmi") || b.includes("mijia")) return "xiaomi";
  if (b.includes("huawei") || b.includes("honor")) return "huawei";
  if (b.includes("oppo") || b.includes("oneplus") || b.includes("realme")) return "oppo";
  if (b.includes("vivo") || b.includes("iqoo")) return "vivo";
  return "unknown";
};

const setMonitor = async (addr, on) => {
  await fetch(`/monitor?addr=${encodeURIComponent(addr)}&on=${on ? 1 : 0}`);
  await loadData();
};

const forceScan = async () => {
  if (busy) return;
  busy = true;
  document.getElementById("targetStatus").textContent = "扫描中";
  await fetch("/scan");
  busy = false;
  await loadData();
};

const clearMonitors = async () => {
  await fetch("/clear");
  await loadData();
};

const render = (data) => {
  document.getElementById("deviceCount").textContent = `${data.deviceCount} 个`;
  document.getElementById("monitoredCount").textContent = `${data.monitoredCount} 个`;
  document.getElementById("targetStatus").textContent = data.monitoredFound ? "已发现" : (data.monitoredCount ? "未发现" : "未选择");
  document.getElementById("targetDistance").textContent = data.monitoredFound ? data.bestDistance : "-";

  const rows = document.getElementById("rows");
  if (!data.devices.length) {
    rows.innerHTML = '<tr><td colspan="10">本轮扫描没有发现 BLE 广播</td></tr>';
    return;
  }

  rows.innerHTML = data.devices.map(d => `
    <tr class="${d.monitored ? "monitored" : ""}">
      <td><input type="checkbox" ${d.monitored ? "checked" : ""} onchange="setMonitor('${esc(d.address)}', this.checked)"></td>
      <td class="${rssiClass(d.rssi)}">${d.rssi} dBm</td>
      <td>${esc(d.distance)}</td>
      <td><span class="badge ${brandClass(d.brand)}">${esc(d.brand)}</span></td>
      <td>${esc(d.type)}</td>
      <td>${esc(d.name || "未广播名称")}</td>
      <td>${esc(d.address)}</td>
      <td>${esc(d.company || "-")}</td>
      <td>${esc(d.serviceUUID || "-")}</td>
      <td class="muted">${esc(d.manufacturer || "-")}</td>
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
  updateBestMonitored();

  String json = "{";
  json += "\"deviceCount\":" + String(deviceCount) + ",";
  json += "\"monitoredCount\":" + String(monitoredCount) + ",";
  json += "\"monitoredFound\":" + String(monitoredFound ? "true" : "false") + ",";
  json += "\"bestRSSI\":" + String(bestMonitoredRSSI) + ",";
  json += "\"bestDistance\":\"" + jsonEscape(distanceText(bestMonitoredDistance)) + "\",";
  json += "\"bestName\":\"" + jsonEscape(bestMonitoredName) + "\",";
  json += "\"bestAddress\":\"" + jsonEscape(bestMonitoredAddr) + "\",";
  json += "\"devices\":[";

  for (int i = 0; i < deviceCount; i++) {
    if (i > 0) json += ",";
    json += "{";
    json += "\"address\":\"" + jsonEscape(devices[i].address) + "\",";
    json += "\"name\":\"" + jsonEscape(devices[i].name) + "\",";
    json += "\"rssi\":" + String(devices[i].rssi) + ",";
    json += "\"distance\":\"" + jsonEscape(distanceText(devices[i].distanceM)) + "\",";
    json += "\"brand\":\"" + jsonEscape(devices[i].brand) + "\",";
    json += "\"type\":\"" + jsonEscape(devices[i].deviceType) + "\",";
    json += "\"company\":\"" + jsonEscape(devices[i].company) + "\",";
    json += "\"companyId\":" + String(devices[i].companyId) + ",";
    json += "\"serviceUUID\":\"" + jsonEscape(devices[i].serviceUUID) + "\",";
    json += "\"manufacturer\":\"" + jsonEscape(devices[i].manufacturerHex) + "\",";
    json += "\"monitored\":" + String(devices[i].monitored ? "true" : "false");
    json += "}";
  }

  json += "]}";
  server.send(200, "application/json; charset=utf-8", json);
}

void handleMonitor() {
  String addr = server.arg("addr");
  bool on = server.arg("on") == "1" || server.arg("on") == "true";

  if (on) {
    if (!addMonitored(addr)) {
      server.send(507, "text/plain; charset=utf-8", "monitor list full");
      return;
    }
  } else {
    removeMonitored(addr);
  }

  saveMonitors();

  for (int i = 0; i < deviceCount; i++) {
    devices[i].monitored = isMonitored(devices[i].address);
  }

  updateBestMonitored();
  drawOled();
  server.send(200, "text/plain; charset=utf-8", "OK");
}

void handleClear() {
  monitoredCount = 0;
  saveMonitors();
  for (int i = 0; i < deviceCount; i++) devices[i].monitored = false;
  updateBestMonitored();
  drawOled();
  server.send(200, "text/plain; charset=utf-8", "OK");
}

void handleScan() {
  performScan();
  server.send(200, "text/plain; charset=utf-8", "OK");
}

// ================== Arduino ==================
void setup() {
  Serial.begin(115200);
  delay(800);

  pinMode(LAMP_PIN, OUTPUT);
  lampWrite(false);
  pinMode(BOARD_LED_PIN, OUTPUT);
  boardLedWrite(false);
  pinMode(BUZZER_PIN, OUTPUT);
  buzzerWrite(false);
  setupConfiguredOutputs();

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
  loadMonitors();

  server.on("/", handleRoot);
  server.on("/api", handleApi);
  server.on("/monitor", handleMonitor);
  server.on("/clear", handleClear);
  server.on("/scan", handleScan);
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

