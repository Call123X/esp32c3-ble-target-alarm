#include "version_select.h"

#if BLE_ALARM_VERSION == 4

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <math.h>

#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>

#define BOARD_LED_PIN 8
#define BOARD_LED_ACTIVE_LOW 1

const char* AP_SSID = "ESP32C3-BLE";
const char* AP_PASS = "12345678";

#define SCAN_SECONDS 3
#define SCAN_INTERVAL_MS 6000
#define MAX_DEVICES 80
#define RSSI_AT_1M -59
#define PATH_LOSS_N 2.2f
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

struct DeviceInfo {
  String address;
  String name;
  int rssi;
  float distanceM;
};

TargetOutputRule targetRules[] = {
  // address              label        GPIO  type                    activeLow  toneHz  runtime fields...
  {"aa:bb:cc:dd:ee:01", "target-1",    3,   OUTPUT_LAMP,            false,     0,      false, -999, 0.0f, false, 0},
  {"aa:bb:cc:dd:ee:02", "target-2",    4,   OUTPUT_PASSIVE_BUZZER,  false,     2300,   false, -999, 0.0f, false, 0},
  {"aa:bb:cc:dd:ee:03", "target-3",    7,   OUTPUT_LAMP,            false,     0,      false, -999, 0.0f, false, 0},
};

const int TARGET_RULE_COUNT = sizeof(targetRules) / sizeof(targetRules[0]);

WebServer server(80);
BLEScan* bleScan = nullptr;

DeviceInfo devices[MAX_DEVICES];
int deviceCount = 0;

unsigned long lastScanMs = 0;
unsigned long lastBoardBlinkMs = 0;
bool boardState = false;

String normalizeAddr(String address) {
  address.toLowerCase();
  return address;
}

void boardLedWrite(bool on) {
#if BOARD_LED_ACTIVE_LOW
  digitalWrite(BOARD_LED_PIN, on ? LOW : HIGH);
#else
  digitalWrite(BOARD_LED_PIN, on ? HIGH : LOW);
#endif
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

String jsonEscape(String s) {
  String out = "";
  for (int i = 0; i < s.length(); i++) {
    uint8_t c = (uint8_t)s[i];
    if (c == '\\') out += "\\\\";
    else if (c == '"') out += "\\\"";
    else if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else if (c == '\t') out += "\\t";
    else if (c < 0x20) {
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

    if (type == 0x09 && valueLen > 0) return bytesToString(value, valueLen);
    if (type == 0x08 && valueLen > 0 && shortName.length() == 0) shortName = bytesToString(value, valueLen);

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
    float distanceM = estimateDistanceM(rssi);
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
      devices[index].distanceM = distanceM;
    }

    updateConfiguredTarget(address, devices[index].rssi, devices[index].distanceM);
  }
};

void performScan() {
  deviceCount = 0;
  resetConfiguredTargets();
  bleScan->start(SCAN_SECONDS, false);
  bleScan->clearResults();
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

void updateBoardLed(bool anyFound, unsigned long now) {
  if (!anyFound) {
    boardLedWrite(false);
    boardState = false;
    return;
  }

  if (now - lastBoardBlinkMs >= 260) {
    lastBoardBlinkMs = now;
    boardState = !boardState;
    boardLedWrite(boardState);
  }
}

void handleRoot() {
  String html = R"rawliteral(
<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>ESP32-C3 无屏多接口蓝牙告警</title>
  <style>
    body{margin:0;padding:14px;font-family:Arial,"Microsoft YaHei",sans-serif;background:#0f172a;color:#e5e7eb}
    h1{font-size:20px;margin:0 0 12px}.panel{border:1px solid #334155;border-radius:8px;padding:12px;margin-bottom:12px;background:#111827}
    button{border:0;border-radius:7px;padding:9px 12px;color:white;background:#2563eb;margin:4px 6px 4px 0}
    table{width:100%;min-width:720px;border-collapse:collapse;font-size:13px}th,td{border-bottom:1px solid #334155;padding:8px;text-align:left}.wrap{overflow-x:auto}
    .on{color:#f59e0b;font-weight:700}.muted{color:#94a3b8}
  </style>
</head>
<body>
  <h1>ESP32-C3 无屏多接口蓝牙告警</h1>
  <div class="panel"><div>附近设备：<b id="count">-</b>　命中规则：<b id="hits">-</b></div><button onclick="scanNow()">立即扫描</button></div>
  <div class="panel wrap"><h3>规则</h3><table><thead><tr><th>状态</th><th>备注</th><th>MAC</th><th>GPIO</th><th>类型</th><th>RSSI</th><th>距离</th></tr></thead><tbody id="rules"></tbody></table></div>
  <div class="panel wrap"><h3>附近 BLE</h3><table><thead><tr><th>RSSI</th><th>距离</th><th>名称</th><th>MAC</th></tr></thead><tbody id="devices"></tbody></table></div>
<script>
let busy=false;
const esc=(v)=>String(v??"").replace(/[&<>"']/g,c=>({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;","'":"&#39;"}[c]));
const scanNow=async()=>{if(busy)return;busy=true;await fetch("/scan");busy=false;await load();};
const render=(d)=>{
  document.getElementById("count").textContent=d.deviceCount;
  document.getElementById("hits").textContent=d.hitCount;
  document.getElementById("rules").innerHTML=d.rules.map(r=>`<tr><td>${r.found?'<span class="on">发现</span>':'<span class="muted">未发现</span>'}</td><td>${esc(r.label)}</td><td>${esc(r.address)}</td><td>GPIO${r.pin}</td><td>${esc(r.type)}</td><td>${r.rssi}</td><td>${esc(r.distance)}</td></tr>`).join("");
  document.getElementById("devices").innerHTML=d.devices.map(x=>`<tr><td>${x.rssi} dBm</td><td>${esc(x.distance)}</td><td>${esc(x.name||"未广播名称")}</td><td>${esc(x.address)}</td></tr>`).join("")||'<tr><td colspan="4">暂无设备</td></tr>';
};
const load=async()=>{if(busy)return;render(await (await fetch("/api")).json());};
load();setInterval(load,2500);
</script>
</body>
</html>
)rawliteral";

  server.send(200, "text/html; charset=utf-8", html);
}

void handleApi() {
  int hitCount = 0;

  String json = "{";
  json += "\"deviceCount\":" + String(deviceCount) + ",";
  json += "\"rules\":[";

  for (int i = 0; i < TARGET_RULE_COUNT; i++) {
    TargetOutputRule& rule = targetRules[i];
    if (rule.found) hitCount++;
    if (i > 0) json += ",";
    json += "{";
    json += "\"address\":\"" + jsonEscape(rule.address) + "\",";
    json += "\"label\":\"" + jsonEscape(rule.label) + "\",";
    json += "\"pin\":" + String(rule.pin) + ",";
    json += "\"type\":\"" + String(rule.type == OUTPUT_PASSIVE_BUZZER ? "蜂鸣器" : "灯") + "\",";
    json += "\"found\":" + String(rule.found ? "true" : "false") + ",";
    json += "\"rssi\":" + String(rule.rssi) + ",";
    json += "\"distance\":\"" + jsonEscape(distanceText(rule.distanceM)) + "\"";
    json += "}";
  }

  json += "],\"hitCount\":" + String(hitCount) + ",";
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

void setup() {
  Serial.begin(115200);
  delay(800);

  pinMode(BOARD_LED_PIN, OUTPUT);
  boardLedWrite(false);
  setupConfiguredOutputs();

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);

  server.on("/", handleRoot);
  server.on("/api", handleApi);
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

  unsigned long now = millis();
  bool anyFound = updateConfiguredOutputs(now);
  updateBoardLed(anyFound, now);

  if (now - lastScanMs >= SCAN_INTERVAL_MS) {
    performScan();
    lastScanMs = millis();
  }
}

#endif
