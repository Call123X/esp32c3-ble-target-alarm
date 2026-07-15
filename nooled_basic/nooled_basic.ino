
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <math.h>

#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>

#define BUZZER_PIN 1
#define LAMP_PIN 2
#define BOARD_LED_PIN 8

#define LAMP_ACTIVE_LOW 0
#define BOARD_LED_ACTIVE_LOW 1

const char* AP_SSID = "ESP32C3-BLE";
const char* AP_PASS = "12345678";

#define SCAN_SECONDS 5
#define SCAN_INTERVAL_MS 6000
#define MAX_DEVICES 80
#define MAX_MONITORED 16
#define ALERT_MIN_RSSI -98
#define TARGET_LOST_TIMEOUT_MS 20000UL
#define RSSI_AT_1M -59
#define PATH_LOSS_N 2.2f
#define BUZZER_FREQ_HZ 2300

struct DeviceInfo {
  String address;
  String name;
  int rssi;
  float distanceM;
  bool monitored;
};

WebServer server(80);
Preferences prefs;
BLEScan* bleScan = nullptr;

DeviceInfo devices[MAX_DEVICES];
int deviceCount = 0;

String monitoredAddresses[MAX_MONITORED];
int monitoredCount = 0;

bool monitoredFound = false;
int bestRSSI = -999;
float bestDistanceM = 0.0f;
String bestName = "";
String bestAddress = "";
unsigned long lastTargetSeenMs = 0;

unsigned long lastScanMs = 0;
unsigned long lastBlinkMs = 0;
unsigned long lastBeepMs = 0;
bool lampState = false;
bool beepState = false;

String normalizeAddr(String address) {
  address.toLowerCase();
  return address;
}

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
    if (addr.length() > 0 && !isMonitored(addr)) monitoredAddresses[monitoredCount++] = addr;
    if (comma < 0) break;
    start = comma + 1;
  }
}

void updateBestMonitored() {
  if (monitoredCount == 0) {
    monitoredFound = false;
    bestRSSI = -999;
    bestDistanceM = 0.0f;
    bestName = "";
    bestAddress = "";
    lastTargetSeenMs = 0;
    return;
  }

  bool foundNow = false;
  int scanBestRSSI = -999;
  float scanBestDistance = 0.0f;
  String scanBestName = "";
  String scanBestAddress = "";

  for (int i = 0; i < deviceCount; i++) {
    devices[i].monitored = isMonitored(devices[i].address);
    if (devices[i].monitored && devices[i].rssi >= ALERT_MIN_RSSI && devices[i].rssi > scanBestRSSI) {
      foundNow = true;
      scanBestRSSI = devices[i].rssi;
      scanBestDistance = devices[i].distanceM;
      scanBestName = devices[i].name.length() ? devices[i].name : devices[i].address;
      scanBestAddress = devices[i].address;
    }
  }

  if (foundNow) {
    monitoredFound = true;
    bestRSSI = scanBestRSSI;
    bestDistanceM = scanBestDistance;
    bestName = scanBestName;
    bestAddress = scanBestAddress;
    lastTargetSeenMs = millis();
    return;
  }

  if (lastTargetSeenMs != 0 && millis() - lastTargetSeenMs <= TARGET_LOST_TIMEOUT_MS) {
    monitoredFound = true;
    return;
  }

  monitoredFound = false;
  bestRSSI = -999;
  bestDistanceM = 0.0f;
  bestName = "";
  bestAddress = "";
}

void sortDevices() {
  for (int i = 0; i < deviceCount - 1; i++) {
    for (int j = i + 1; j < deviceCount; j++) {
      bool swapNeeded = devices[j].monitored && !devices[i].monitored;
      if (devices[j].monitored == devices[i].monitored && devices[j].rssi > devices[i].rssi) swapNeeded = true;
      if (swapNeeded) {
        DeviceInfo tmp = devices[i];
        devices[i] = devices[j];
        devices[j] = tmp;
      }
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
    int index = findDevice(address);
    if (index < 0) {
      if (deviceCount >= MAX_DEVICES) return;
      index = deviceCount++;
      devices[index].address = address;
      devices[index].name = "";
      devices[index].rssi = -999;
      devices[index].distanceM = 0.0f;
      devices[index].monitored = false;
    }

    if (name.length() > 0) devices[index].name = name;
    if (rssi > devices[index].rssi) {
      devices[index].rssi = rssi;
      devices[index].distanceM = estimateDistanceM(rssi);
    }
    devices[index].monitored = isMonitored(address);
  }
};

void performScan() {
  deviceCount = 0;

  bleScan->start(SCAN_SECONDS, false);
  bleScan->clearResults();
  updateBestMonitored();
  sortDevices();
}

void updateAlarm() {
  unsigned long now = millis();

  if (!monitoredFound) {
    lampWrite(false);
    boardLedWrite(false);
    buzzerWrite(false);
    lampState = false;
    beepState = false;
    return;
  }

  if (now - lastBlinkMs >= 260) {
    lastBlinkMs = now;
    lampState = !lampState;
    lampWrite(lampState);
    boardLedWrite(lampState);
  }

  int intervalMs = map(constrain(bestRSSI, -95, -45), -95, -45, 1100, 360);
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

void handleRoot() {
  String html = R"rawliteral(
<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>ESP32-C3 无屏蓝牙告警</title>
  <style>
    body{margin:0;padding:14px;font-family:Arial,"Microsoft YaHei",sans-serif;background:#0f172a;color:#e5e7eb}
    h1{font-size:20px;margin:0 0 12px}.panel{border:1px solid #334155;border-radius:8px;padding:12px;margin-bottom:12px;background:#111827}
    button{border:0;border-radius:7px;padding:9px 12px;color:white;background:#2563eb;margin:4px 6px 4px 0}
    button.warn{background:#dc2626}table{width:100%;min-width:720px;border-collapse:collapse;font-size:13px}th,td{border-bottom:1px solid #334155;padding:8px;text-align:left}
    .wrap{overflow-x:auto}.on{color:#f59e0b;font-weight:700}.ok{color:#22c55e;font-weight:700}.muted{color:#94a3b8}
    input{width:20px;height:20px;accent-color:#22c55e}
  </style>
</head>
<body>
  <h1>ESP32-C3 无屏蓝牙告警</h1>
  <div class="panel">
    <div>附近设备：<b id="count">-</b>　已监测：<b id="monitors">-</b>　状态：<b id="status">-</b>　距离：<b id="distance">-</b></div>
    <button onclick="scanNow()">立即扫描</button><button class="warn" onclick="clearAll()">清空监测</button>
  </div>
  <div class="panel wrap"><table><thead><tr><th>监测</th><th>RSSI</th><th>距离</th><th>名称</th><th>MAC</th></tr></thead><tbody id="rows"></tbody></table></div>
<script>
let busy=false;
const esc=(v)=>String(v??"").replace(/[&<>"']/g,c=>({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;","'":"&#39;"}[c]));
const setMon=async(a,on)=>{await fetch(`/monitor?addr=${encodeURIComponent(a)}&on=${on?1:0}`);await load();};
const scanNow=async()=>{if(busy)return;busy=true;document.getElementById("status").textContent="扫描中";await fetch("/scan");busy=false;await load();};
const clearAll=async()=>{await fetch("/clear");await load();};
const render=(d)=>{
  document.getElementById("count").textContent=d.deviceCount;
  document.getElementById("monitors").textContent=d.monitoredCount;
  document.getElementById("status").innerHTML=d.found?'<span class="on">已发现</span>':(d.monitoredCount?'<span class="muted">未发现</span>':'未选择');
  document.getElementById("distance").textContent=d.found?d.bestDistance:"-";
  document.getElementById("rows").innerHTML=d.devices.map(x=>`<tr><td><input type="checkbox" ${x.monitored?"checked":""} onchange="setMon('${esc(x.address)}',this.checked)"></td><td>${x.rssi} dBm</td><td>${esc(x.distance)}</td><td>${esc(x.name||"未广播名称")}</td><td>${esc(x.address)}</td></tr>`).join("")||'<tr><td colspan="5">暂无设备</td></tr>';
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
  updateBestMonitored();

  String json = "{";
  json += "\"deviceCount\":" + String(deviceCount) + ",";
  json += "\"monitoredCount\":" + String(monitoredCount) + ",";
  json += "\"found\":" + String(monitoredFound ? "true" : "false") + ",";
  json += "\"bestRSSI\":" + String(bestRSSI) + ",";
  json += "\"bestDistance\":\"" + jsonEscape(distanceText(bestDistanceM)) + "\",";
  json += "\"bestName\":\"" + jsonEscape(bestName) + "\",";
  json += "\"bestAddress\":\"" + jsonEscape(bestAddress) + "\",";
  json += "\"devices\":[";

  for (int i = 0; i < deviceCount; i++) {
    if (i > 0) json += ",";
    json += "{";
    json += "\"address\":\"" + jsonEscape(devices[i].address) + "\",";
    json += "\"name\":\"" + jsonEscape(devices[i].name) + "\",";
    json += "\"rssi\":" + String(devices[i].rssi) + ",";
    json += "\"distance\":\"" + jsonEscape(distanceText(devices[i].distanceM)) + "\",";
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
  updateBestMonitored();
  server.send(200, "text/plain; charset=utf-8", "OK");
}

void handleClear() {
  monitoredCount = 0;
  saveMonitors();
  updateBestMonitored();
  server.send(200, "text/plain; charset=utf-8", "OK");
}

void handleScan() {
  performScan();
  server.send(200, "text/plain; charset=utf-8", "OK");
}

void setup() {
  Serial.begin(115200);
  delay(800);

  pinMode(LAMP_PIN, OUTPUT);
  lampWrite(false);
  pinMode(BOARD_LED_PIN, OUTPUT);
  boardLedWrite(false);
  pinMode(BUZZER_PIN, OUTPUT);
  buzzerWrite(false);

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);

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
