
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
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
#define MAX_RULES 8
#define RSSI_AT_1M -59
#define PATH_LOSS_N 2.2f
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

struct DeviceInfo {
  String address;
  String name;
  int rssi;
  float distanceM;
};

TargetOutputRule targetRules[MAX_RULES];
const int TARGET_RULE_COUNT = MAX_RULES;

WebServer server(80);
Preferences prefs;
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
    String ruleAddress = normalizeAddr(targetRules[i].address);
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
    button.del{background:#dc2626}button.mini{padding:6px 8px;font-size:12px}input,select{background:#0b1220;color:#e5e7eb;border:1px solid #334155;border-radius:6px;padding:6px;max-width:150px}
    table{width:100%;min-width:980px;border-collapse:collapse;font-size:13px}th,td{border-bottom:1px solid #334155;padding:8px;text-align:left}.wrap{overflow-x:auto}
    .on{color:#f59e0b;font-weight:700}.muted{color:#94a3b8}.small{font-size:12px;color:#94a3b8;line-height:1.6}
  </style>
</head>
<body>
  <h1>ESP32-C3 无屏多接口蓝牙告警</h1>
  <div class="panel"><div>附近设备：<b id="count">-</b>　命中规则：<b id="hits">-</b></div><button onclick="scanNow()">立即扫描</button></div>
  <div class="panel wrap"><h3>网页配置规则</h3><div class="small">选择蓝牙 MAC，填 GPIO，选择这个 GPIO 是灯还是无源蜂鸣器。保存后会写入 ESP32-C3，重启还在。</div><table><thead><tr><th>状态</th><th>备注</th><th>MAC</th><th>GPIO</th><th>类型</th><th>低电平</th><th>频率</th><th>RSSI</th><th>距离</th><th>操作</th></tr></thead><tbody id="rules"></tbody></table></div>
  <div class="panel wrap"><h3>附近 BLE</h3><table><thead><tr><th>RSSI</th><th>距离</th><th>名称</th><th>MAC</th><th>操作</th></tr></thead><tbody id="devices"></tbody></table></div>
<script>
let busy=false;
const esc=(v)=>String(v??"").replace(/[&<>"']/g,c=>({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;","'":"&#39;"}[c]));
const enc=(v)=>encodeURIComponent(String(v??""));
const scanNow=async()=>{if(busy)return;busy=true;await fetch("/scan");busy=false;await load();};
const firstEmpty=(rules)=>{let i=rules.findIndex(r=>!r.enabled);return i<0?0:i;};
const fillFromDevice=(addr,name)=>{const rules=window.lastData.rules;const i=firstEmpty(rules);document.getElementById(`addr${i}`).value=addr;document.getElementById(`label${i}`).value=name||addr;document.getElementById(`en${i}`).checked=true;window.scrollTo(0,0);};
const saveRule=async(i)=>{
  const q=new URLSearchParams({
    i,
    addr:document.getElementById(`addr${i}`).value.trim(),
    label:document.getElementById(`label${i}`).value.trim(),
    pin:document.getElementById(`pin${i}`).value,
    type:document.getElementById(`type${i}`).value,
    low:document.getElementById(`low${i}`).checked?1:0,
    hz:document.getElementById(`hz${i}`).value,
    en:document.getElementById(`en${i}`).checked?1:0
  });
  await fetch(`/rule?${q.toString()}`);
  await load();
};
const delRule=async(i)=>{await fetch(`/delete?i=${i}`);await load();};
const render=(d)=>{
  window.lastData=d;
  document.getElementById("count").textContent=d.deviceCount;
  document.getElementById("hits").textContent=d.hitCount;
  document.getElementById("rules").innerHTML=d.rules.map(r=>`<tr><td>${r.found?'<span class="on">发现</span>':'<span class="muted">未发现</span>'}<br><label><input id="en${r.index}" type="checkbox" ${r.enabled?"checked":""}>启用</label></td><td><input id="label${r.index}" value="${esc(r.label)}"></td><td><input id="addr${r.index}" value="${esc(r.address)}" placeholder="aa:bb:cc:dd:ee:ff"></td><td><input id="pin${r.index}" type="number" min="0" max="21" value="${r.pin}"></td><td><select id="type${r.index}"><option value="lamp" ${r.type==="灯"?"selected":""}>灯</option><option value="buzzer" ${r.type==="蜂鸣器"?"selected":""}>蜂鸣器</option></select></td><td><input id="low${r.index}" type="checkbox" ${r.activeLow?"checked":""}></td><td><input id="hz${r.index}" type="number" value="${r.toneHz}"></td><td>${r.rssi}</td><td>${esc(r.distance)}</td><td><button class="mini" onclick="saveRule(${r.index})">保存</button><button class="mini del" onclick="delRule(${r.index})">删除</button></td></tr>`).join("");
  document.getElementById("devices").innerHTML=d.devices.map(x=>`<tr><td>${x.rssi} dBm</td><td>${esc(x.distance)}</td><td>${esc(x.name||"未广播名称")}</td><td>${esc(x.address)}</td><td><button class="mini" onclick="fillFromDevice(decodeURIComponent('${enc(x.address)}'),decodeURIComponent('${enc(x.name||"")}'))">填到规则</button></td></tr>`).join("")||'<tr><td colspan="5">暂无设备</td></tr>';
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

void setup() {
  Serial.begin(115200);
  delay(800);

  pinMode(BOARD_LED_PIN, OUTPUT);
  boardLedWrite(false);
  prefs.begin("blemulti", false);
  loadRules();
  setupConfiguredOutputs();

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);

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

  unsigned long now = millis();
  bool anyFound = updateConfiguredOutputs(now);
  updateBoardLed(anyFound, now);

  if (now - lastScanMs >= SCAN_INTERVAL_MS) {
    performScan();
    lastScanMs = millis();
  }
}
