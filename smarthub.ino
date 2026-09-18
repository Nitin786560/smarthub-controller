#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266httpUpdate.h>
#include <WiFiClientSecure.h>
#include <EEPROM.h>

// ============================================================================
// HARDWARE & REPOSITORY CONFIGURATION
// ============================================================================
const String HARDWARE_VERSION = "1.0.1"; // Active device signature
const char* GITHUB_REPO       = "Nitin786560/smarthub-controller";

// GitHub release asset direct URL targets
const String OTA_BIN_URL = "http://github.com/" + String(GITHUB_REPO) + "/releases/latest/download/firmware.bin";
const String OTA_MD5_URL = "http://github.com/" + String(GITHUB_REPO) + "/releases/latest/download/firmware.bin.md5";

// Network fallback profile defaults
const char* DEFAULT_HOME_SSID   = "YOUR_HOME_WIFI";
const char* DEFAULT_HOME_PASS   = "YOUR_HOME_PASS";
const char* FALLBACK_REDMI_SSID = "Redmi";
const char* FALLBACK_REDMI_PASS = "1234567890";
const char* AP_SSID             = "SmartHub-NodeMCU";
const char* AP_PASS             = "1234567890";

// Internal EEPROM sector offsets
#define EEPROM_SIZE       128
#define EEPROM_FLAG_ADDR  0
#define EEPROM_SSID_ADDR  1
#define EEPROM_PASS_ADDR  33
#define EEPROM_MAGIC_BYTE 0x4A

ESP8266WebServer server(80);
unsigned long lastOTACheck = 0;
const unsigned long OTA_CHECK_INTERVAL = 60000; // 60-second cycle

// ============================================================================
// CORS MANAGEMENT (Enables cross-origin dashboard access)
// ============================================================================
void applyCORS() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
}

void handleOptions() {
  applyCORS();
  server.send(204);
}

// ============================================================================
// EEPROM STORAGE ROUTINES
// ============================================================================
void readEEPROMCreds(char* ssid, char* pass) {
  if (EEPROM.read(EEPROM_FLAG_ADDR) == EEPROM_MAGIC_BYTE) {
    for (int i = 0; i < 32; ++i) ssid[i] = char(EEPROM.read(EEPROM_SSID_ADDR + i));
    for (int i = 0; i < 32; ++i) pass[i] = char(EEPROM.read(EEPROM_PASS_ADDR + i));
  } else {
    ssid[0] = '\0';
    pass[0] = '\0';
  }
}

void writeEEPROMCreds(const String& ssid, const String& pass) {
  EEPROM.write(EEPROM_FLAG_ADDR, EEPROM_MAGIC_BYTE);
  for (int i = 0; i < 32; ++i) {
    EEPROM.write(EEPROM_SSID_ADDR + i, i < ssid.length() ? ssid[i] : '\0');
  }
  for (int i = 0; i < 32; ++i) {
    EEPROM.write(EEPROM_PASS_ADDR + i, i < pass.length() ? pass[i] : '\0');
  }
  EEPROM.commit();
}

// ============================================================================
// MULTI-TIER RESILIENT HANDSHAKE
// ============================================================================
bool tryConnect(const char* ssid, const char* pass, int timeoutSec = 8) {
  if (strlen(ssid) == 0) return false;
  Serial.printf("[WiFi] Probing: %s\n", ssid);
  WiFi.begin(ssid, pass);

  int elapsed = 0;
  while (WiFi.status() != WL_CONNECTED && elapsed < (timeoutSec * 2)) {
    delay(500);
    Serial.print(".");
    elapsed++;
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[WiFi] Connected! IP Address: %s\n", WiFi.localIP().toString().c_str());
    return true;
  }
  return false;
}

void initNetwork() {
  WiFi.mode(WIFI_AP_STA);

  char eepromSSID[32];
  char eepromPASS[32];
  readEEPROMCreds(eepromSSID, eepromPASS);

  // Priority 1: Home Network -> Priority 2: Redmi Hotspot -> Priority 3: EEPROM
  if (!tryConnect(DEFAULT_HOME_SSID, DEFAULT_HOME_PASS, 8)) {
    if (!tryConnect(FALLBACK_REDMI_SSID, FALLBACK_REDMI_PASS, 8)) {
      if (!tryConnect(eepromSSID, eepromPASS, 8)) {
        Serial.println("[WiFi] All client attempts exhausted. AP mode active.");
      }
    }
  }

  // Standalone Access Point configuration
  IPAddress apIP(192, 168, 4, 1);
  IPAddress netMsk(255, 255, 255, 0);
  WiFi.softAPConfig(apIP, apIP, netMsk);
  WiFi.softAP(AP_SSID, AP_PASS);
  Serial.printf("[WiFi] Standalone AP active: %s (IP: 192.168.4.1)\n", AP_SSID);
}

// ============================================================================
// GITHUB OVER-THE-AIR (OTA) CLIENT
// ============================================================================
void runOTA() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[OTA] Skipped: No internet connectivity.");
    return;
  }

  Serial.println("[OTA] Pulling remote release from GitHub...");
  WiFiClient client;
  ESPhttpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  ESPhttpUpdate.rebootOnUpdate(true);

  t_httpUpdate_return ret = ESPhttpUpdate.update(client, OTA_BIN_URL);

  switch (ret) {
    case HTTP_UPDATE_FAILED:
      Serial.printf("[OTA] Failed (%d): %s\n", ESPhttpUpdate.getLastError(), ESPhttpUpdate.getLastErrorString().c_str());
      break;
    case HTTP_UPDATE_NO_UPDATES:
      Serial.println("[OTA] Current binary is up to date.");
      break;
    case HTTP_UPDATE_OK:
      Serial.println("[OTA] Flash success. Rebooting device...");
      break;
  }
}

// ============================================================================
// ARDUINO UNO COMMAND DISPATCHER
// ============================================================================
String forwardCommandToSlave(String targetIp, String endpoint) {
  if (WiFi.status() != WL_CONNECTED && WiFi.softAPgetStationNum() == 0) {
    return "{\"status\":\"error\",\"message\":\"Mesh disconnected\"}";
  }
  WiFiClient client;
  HTTPClient http;
  http.begin(client, "http://" + targetIp + endpoint);
  int httpCode = http.GET();
  String response = (httpCode > 0) ? http.getString() : "{\"status\":\"timeout\"}";
  http.end();
  return response;
}

// ============================================================================
// REST API HANDLERS
// ============================================================================
void handleStatus() {
  applyCORS();
  String activeIP = (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
  String activeSSID = (WiFi.status() == WL_CONNECTED) ? WiFi.SSID() : String(AP_SSID);

  String json = "{";
  json += "\"version\":\"" + HARDWARE_VERSION + "\",";
  json += "\"ip\":\"" + activeIP + "\",";
  json += "\"ssid\":\"" + activeSSID + "\",";
  json += "\"mesh_clients\":" + String(WiFi.softAPgetStationNum()) + ",";
  json += "\"uptime_ms\":" + String(millis()) + ",";
  json += "\"status\":\"online\"";
  json += "}";

  server.send(200, "application/json", json);
}

void handleRelayRoute() {
  applyCORS();
  String action = server.hasArg("action") ? server.arg("action") : "status";
  String slaveIp = "192.168.4.20"; // Destination IP for the Arduino Uno node
  String response = forwardCommandToSlave(slaveIp, "/relay?state=" + action);
  server.send(200, "application/json", response);
}

void handleSaveWiFi() {
  applyCORS();
  if (server.hasArg("ssid") && server.hasArg("pass")) {
    writeEEPROMCreds(server.arg("ssid"), server.arg("pass"));
    server.send(200, "text/plain", "Credentials written to EEPROM. Rebooting...");
    delay(1000);
    ESP.restart();
  } else {
    server.send(400, "text/plain", "Missing SSID or Password");
  }
}

void handleTriggerOTA() {
  applyCORS();
  server.send(200, "text/plain", "Manual OTA triggered. Inspecting release binary...");
  runOTA();
}

// ============================================================================
// EMBEDDED DASHBOARD (Responsive HTML + JS)
// ============================================================================
void handleRoot() {
  applyCORS();
  String page = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>SmartHub Controller</title>
  <style>
    :root { --bg: #0d1117; --panel: #161b22; --border: #30363d; --text: #c9d1d9; --accent: #238636; --danger: #da3633; --blue: #1f6feb; }
    * { box-sizing: border-box; margin: 0; padding: 0; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; }
    body { background: var(--bg); color: var(--text); display: flex; justify-content: center; padding: 24px 12px; }
    .container { width: 100%; max-width: 480px; display: flex; flex-direction: column; gap: 16px; }
    .card { background: var(--panel); border: 1px solid var(--border); border-radius: 12px; padding: 20px; box-shadow: 0 4px 12px rgba(0,0,0,0.3); }
    h2, h3 { color: #f0f6fc; margin-bottom: 12px; font-weight: 600; }
    .meta-row { display: flex; justify-content: space-between; padding: 8px 0; border-bottom: 1px solid var(--border); font-size: 14px; }
    .meta-row:last-child { border-bottom: none; }
    .badge { background: #21262d; border: 1px solid var(--border); padding: 2px 8px; border-radius: 6px; font-family: monospace; font-weight: bold; color: #58a6ff; }
    .btn-group { display: flex; gap: 10px; margin-top: 8px; }
    button { flex: 1; padding: 12px; border: none; border-radius: 6px; font-weight: bold; cursor: pointer; color: #fff; transition: opacity 0.2s; }
    button:active { opacity: 0.8; }
    .btn-green { background: var(--accent); }
    .btn-red { background: var(--danger); }
    .btn-blue { background: var(--blue); width: 100%; }
    input[type="text"], input[type="password"] { width: 100%; padding: 10px 12px; margin-bottom: 10px; background: var(--bg); border: 1px solid var(--border); border-radius: 6px; color: #fff; }
    #log { font-family: monospace; font-size: 12px; color: #8b949e; margin-top: 8px; min-height: 16px; text-align: center; }
  </style>
</head>
<body>
  <div class="container">
    <div class="card">
      <h2>SmartHub Master</h2>
      <div class="meta-row"><span>Firmware:</span><span class="badge" id="v-val">Loading...</span></div>
      <div class="meta-row"><span>Node IP:</span><span class="badge" id="ip-val">Loading...</span></div>
      <div class="meta-row"><span>Active SSID:</span><span class="badge" id="ssid-val">Loading...</span></div>
      <div class="meta-row"><span>Mesh Clients:</span><span class="badge" id="client-val">0</span></div>
    </div>

    <div class="card">
      <h3>Slave Command Router</h3>
      <p style="font-size:13px; color:#8b949e; margin-bottom:10px;">Routes packets directly to Arduino Uno</p>
      <div class="btn-group">
        <button class="btn-green" onclick="sendRelay('on')">Relay ON</button>
        <button class="btn-red" onclick="sendRelay('off')">Relay OFF</button>
      </div>
    </div>

    <div class="card">
      <h3>Wi-Fi Sector Configuration</h3>
      <form action="/api/save-wifi" method="POST">
        <input type="text" name="ssid" placeholder="Network SSID" required>
        <input type="password" name="pass" placeholder="Password" required>
        <button type="submit" class="btn-green" style="width:100%;">Save to EEPROM & Reboot</button>
      </form>
    </div>

    <div class="card">
      <h3>Worldwide OTA Control</h3>
      <button class="btn-blue" onclick="triggerOTA()">Check & Pull GitHub Release</button>
      <div id="log"></div>
    </div>
  </div>

  <script>
    function updateTelemetry() {
      fetch('/api/status')
        .then(r => r.json())
        .then(d => {
          document.getElementById('v-val').innerText = d.version;
          document.getElementById('ip-val').innerText = d.ip;
          document.getElementById('ssid-val').innerText = d.ssid;
          document.getElementById('client-val').innerText = d.mesh_clients;
        })
        .catch(() => {
          document.getElementById('v-val').innerText = "Offline";
        });
    }

    function sendRelay(act) {
      document.getElementById('log').innerText = "Sending command...";
      fetch('/api/relay?action=' + act)
        .then(r => r.text())
        .then(t => { document.getElementById('log').innerText = "Slave response: " + t; })
        .catch(e => { document.getElementById('log').innerText = "Relay dispatch failed"; });
    }

    function triggerOTA() {
      document.getElementById('log').innerText = "Triggering GitHub pull verification...";
      fetch('/api/trigger-ota')
        .then(r => r.text())
        .then(t => { document.getElementById('log').innerText = t; });
    }

    updateTelemetry();
    setInterval(updateTelemetry, 5000);
  </script>
</body>
</html>
)rawliteral";
  server.send(200, "text/html", page);
}

// ============================================================================
// SYSTEM ENTRY & LOOP
// ============================================================================
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n--- [SmartHub Master Controller Online] ---");
  Serial.printf("Version: %s\n", HARDWARE_VERSION.c_str());

  EEPROM.begin(EEPROM_SIZE);
  initNetwork();

  // Route registration
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/status", HTTP_OPTIONS, handleOptions);
  server.on("/api/relay", HTTP_GET, handleRelayRoute);
  server.on("/api/relay", HTTP_OPTIONS, handleOptions);
  server.on("/api/save-wifi", HTTP_POST, handleSaveWiFi);
  server.on("/api/save-wifi", HTTP_OPTIONS, handleOptions);
  server.on("/api/trigger-ota", HTTP_GET, handleTriggerOTA);
  server.on("/api/trigger-ota", HTTP_OPTIONS, handleOptions);

  server.begin();
  Serial.println("[HTTP] API and Web dashboard online on port 80.");
}

void loop() {
  server.handleClient();

  if (millis() - lastOTACheck > OTA_CHECK_INTERVAL) {
    lastOTACheck = millis();
    runOTA();
  }
}
