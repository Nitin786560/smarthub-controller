#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266httpUpdate.h>
#include <WiFiClientSecure.h>
#include <EEPROM.h>

// ============================================================================
// HARDWARE DEFINITIONS & DEFAULTS
// ============================================================================
const String HARDWARE_VERSION = "1.1.0";
const char* GITHUB_REPO       = "Nitin786560/smarthub-controller";

const char* DEFAULT_HOME_SSID   = "YOUR_HOME_WIFI";
const char* DEFAULT_HOME_PASS   = "YOUR_HOME_PASS";
const char* FALLBACK_REDMI_SSID = "Redmi";
const char* FALLBACK_REDMI_PASS = "1234567890";
const char* AP_SSID             = "SmartHub-NodeMCU";
const char* AP_PASS             = "1234567890";

// EEPROM Memory Allocation Map (256 Bytes)
#define EEPROM_SIZE        256
#define ADDR_MAGIC         0     // 1 byte (0x5B)
#define ADDR_SSID          1     // 32 bytes
#define ADDR_PASS          33    // 32 bytes
#define ADDR_SLAVE_IP      65    // 20 bytes
#define ADDR_OTA_INTERVAL  85    // 4 bytes (unsigned long)
#define EEPROM_MAGIC_BYTE  0x5B

// Global Runtime State
ESP8266WebServer server(80);
unsigned long lastOTACheck = 0;
unsigned long otaIntervalMs = 60000;
char targetSlaveIP[20] = "192.168.4.20";
int currentPWM = 0;

// ============================================================================
// RESILIENT HEADERS & CORS
// ============================================================================
void applyHeaders() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type, Authorization, X-Requested-With");
  server.sendHeader("Access-Control-Allow-Private-Network", "true");
}

void handleOptions() {
  applyHeaders();
  server.send(204);
}

// ============================================================================
// EEPROM MANAGER
// ============================================================================
void loadEEPROMConfig() {
  if (EEPROM.read(ADDR_MAGIC) == EEPROM_MAGIC_BYTE) {
    for (int i = 0; i < 19; ++i) targetSlaveIP[i] = char(EEPROM.read(ADDR_SLAVE_IP + i));
    targetSlaveIP[19] = '\0';

    EEPROM.get(ADDR_OTA_INTERVAL, otaIntervalMs);
    if (otaIntervalMs < 15000 || otaIntervalMs > 86400000) otaIntervalMs = 60000;
  }
}

void writeEEPROMCreds(const String& ssid, const String& pass) {
  EEPROM.write(ADDR_MAGIC, EEPROM_MAGIC_BYTE);
  for (int i = 0; i < 32; ++i) {
    EEPROM.write(ADDR_SSID + i, i < ssid.length() ? ssid[i] : '\0');
  }
  for (int i = 0; i < 32; ++i) {
    EEPROM.write(ADDR_PASS + i, i < pass.length() ? pass[i] : '\0');
  }
  EEPROM.commit();
}

void writeSlaveIP(const String& ip) {
  EEPROM.write(ADDR_MAGIC, EEPROM_MAGIC_BYTE);
  for (int i = 0; i < 19; ++i) {
    EEPROM.write(ADDR_SLAVE_IP + i, i < ip.length() ? ip[i] : '\0');
  }
  EEPROM.commit();
  ip.toCharArray(targetSlaveIP, 20);
}

// ============================================================================
// AUTO-SCANNING MULTI-NETWORK ENGINE
// ============================================================================
void connectToBestAvailableNetwork() {
  WiFi.mode(WIFI_AP_STA);
  Serial.println("[WiFi] Scanning local spectrum for priority networks...");

  char eepromSSID[32] = {0};
  char eepromPASS[32] = {0};
  if (EEPROM.read(ADDR_MAGIC) == EEPROM_MAGIC_BYTE) {
    for (int i = 0; i < 32; ++i) eepromSSID[i] = char(EEPROM.read(ADDR_SSID + i));
    for (int i = 0; i < 32; ++i) eepromPASS[i] = char(EEPROM.read(ADDR_PASS + i));
  }

  int scanResults = WiFi.scanNetworks();
  bool connected = false;

  if (scanResults > 0) {
    Serial.printf("[WiFi] Detected %d visible APs.\n", scanResults);

    // Profile lookup loop: matches strongest known SSID first
    for (int i = 0; i < scanResults; ++i) {
      String ssidFound = WiFi.SSID(i);
      int rssiFound = WiFi.RSSI(i);

      if (ssidFound == DEFAULT_HOME_SSID) {
        Serial.printf("[WiFi] Matched Home AP (%s, %d dBm). Handshaking...\n", ssidFound.c_str(), rssiFound);
        WiFi.begin(DEFAULT_HOME_SSID, DEFAULT_HOME_PASS);
        connected = (WiFi.waitForConnectResult() == WL_CONNECTED);
        if (connected) break;
      } else if (ssidFound == FALLBACK_REDMI_SSID) {
        Serial.printf("[WiFi] Matched Redmi Hotspot (%s, %d dBm). Handshaking...\n", ssidFound.c_str(), rssiFound);
        WiFi.begin(FALLBACK_REDMI_SSID, FALLBACK_REDMI_PASS);
        connected = (WiFi.waitForConnectResult() == WL_CONNECTED);
        if (connected) break;
      } else if (strlen(eepromSSID) > 0 && ssidFound == String(eepromSSID)) {
        Serial.printf("[WiFi] Matched EEPROM Profile (%s, %d dBm). Handshaking...\n", ssidFound.c_str(), rssiFound);
        WiFi.begin(eepromSSID, eepromPASS);
        connected = (WiFi.waitForConnectResult() == WL_CONNECTED);
        if (connected) break;
      }
    }
  }

  WiFi.scanDelete();

  if (connected) {
    Serial.printf("[WiFi] Station Active! IP: %s | Gateway: %s\n", 
                  WiFi.localIP().toString().c_str(), WiFi.gatewayIP().toString().c_str());
  } else {
    Serial.println("[WiFi] Known station networks out of range. Operating as AP.");
  }

  // Failsafe Standalone SoftAP
  IPAddress apIP(192, 168, 4, 1);
  IPAddress netMsk(255, 255, 255, 0);
  WiFi.softAPConfig(apIP, apIP, netMsk);
  WiFi.softAP(AP_SSID, AP_PASS);
  Serial.printf("[WiFi] Master AP Ready: %s (IP: 192.168.4.1)\n", AP_SSID);
}

// ============================================================================
// SECURE GITHUB OTA RUNNER
// ============================================================================
void runOTA(String customUrl = "") {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[OTA] Skipped: Node not connected to external network.");
    return;
  }

  String targetUrl = (customUrl.length() > 0) ? customUrl : 
    ("https://github.com/" + String(GITHUB_REPO) + "/releases/latest/download/firmware.bin");

  Serial.printf("[OTA] Connecting to secure endpoint: %s\n", targetUrl.c_str());

  WiFiClientSecure client;
  client.setInsecure(); // Bypass CA validation on self-contained MCU
  client.setTimeout(15000);

  ESPhttpUpdate.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  ESPhttpUpdate.rebootOnUpdate(true);

  t_httpUpdate_return ret = ESPhttpUpdate.update(client, targetUrl);

  switch (ret) {
    case HTTP_UPDATE_FAILED:
      Serial.printf("[OTA] Failed (%d): %s\n", ESPhttpUpdate.getLastError(), ESPhttpUpdate.getLastErrorString().c_str());
      break;
    case HTTP_UPDATE_NO_UPDATES:
      Serial.println("[OTA] Firmware binary is current.");
      break;
    case HTTP_UPDATE_OK:
      Serial.println("[OTA] Image flashed. Rebooting device...");
      break;
  }
}

// ============================================================================
// ARDUINO UNO WIRELESS COMMAND ROUTER
// ============================================================================
String forwardCommandToSlave(String endpoint) {
  if (WiFi.status() != WL_CONNECTED && WiFi.softAPgetStationNum() == 0) {
    return "{\"status\":\"error\",\"message\":\"Mesh Offline\"}";
  }

  WiFiClient client;
  HTTPClient http;
  String url = "http://" + String(targetSlaveIP) + endpoint;
  http.begin(client, url);
  http.setTimeout(2500);

  int httpCode = http.GET();
  String payload = (httpCode > 0) ? http.getString() : "{\"status\":\"unreachable\",\"code\":" + String(httpCode) + "}";
  http.end();
  return payload;
}

// ============================================================================
// REST API SERVICES
// ============================================================================
void handleStatus() {
  applyHeaders();
  String ipStr = (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
  String ssidStr = (WiFi.status() == WL_CONNECTED) ? WiFi.SSID() : String(AP_SSID);

  String json = "{";
  json += "\"version\":\"" + HARDWARE_VERSION + "\",";
  json += "\"ip\":\"" + ipStr + "\",";
  json += "\"ssid\":\"" + ssidStr + "\",";
  json += "\"rssi\":" + String(WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0) + ",";
  json += "\"mesh_clients\":" + String(WiFi.softAPgetStationNum()) + ",";
  json += "\"slave_ip\":\"" + String(targetSlaveIP) + "\",";
  json += "\"pwm_val\":" + String(currentPWM) + ",";
  json += "\"free_heap\":" + String(ESP.getFreeHeap()) + ",";
  json += "\"uptime_ms\":" + String(millis()) + ",";
  json += "\"status\":\"online\"";
  json += "}";

  server.send(200, "application/json", json);
}

void handleRelay() {
  applyHeaders();
  String state = server.hasArg("state") ? server.arg("state") : "toggle";
  String channel = server.hasArg("ch") ? server.arg("ch") : "1";
  String res = forwardCommandToSlave("/relay?ch=" + channel + "&state=" + state);
  server.send(200, "application/json", res);
}

void handlePWM() {
  applyHeaders();
  if (server.hasArg("val")) {
    currentPWM = constrain(server.arg("val").toInt(), 0, 255);
    String res = forwardCommandToSlave("/pwm?val=" + String(currentPWM));
    server.send(200, "application/json", res);
  } else {
    server.send(400, "text/plain", "Missing val parameter (0-255)");
  }
}

void handleConfigSlave() {
  applyHeaders();
  if (server.hasArg("ip")) {
    writeSlaveIP(server.arg("ip"));
    server.send(200, "application/json", "{\"status\":\"success\",\"slave_ip\":\"" + String(targetSlaveIP) + "\"}");
  } else {
    server.send(400, "text/plain", "Missing ip");
  }
}

void handleSaveWiFi() {
  applyHeaders();
  if (server.hasArg("ssid") && server.hasArg("pass")) {
    writeEEPROMCreds(server.arg("ssid"), server.arg("pass"));
    server.send(200, "text/plain", "Credentials committed. Re-running network discovery...");
    delay(1000);
    ESP.restart();
  } else {
    server.send(400, "text/plain", "Missing SSID or Password");
  }
}

void handleScan() {
  applyHeaders();
  int n = WiFi.scanNetworks();
  String json = "[";
  for (int i = 0; i < n; ++i) {
    if (i > 0) json += ",";
    json += "{\"ssid\":\"" + WiFi.SSID(i) + "\",\"rssi\":" + String(WiFi.RSSI(i)) + ",\"secure\":" + String(WiFi.encryptionType(i) != ENC_TYPE_NONE) + "}";
  }
  json += "]";
  WiFi.scanDelete();
  server.send(200, "application/json", json);
}

void handleTriggerOTA() {
  applyHeaders();
  String customUrl = server.hasArg("url") ? server.arg("url") : "";
  server.send(200, "text/plain", "OTA routine launched. Check serial logs for flashing progress.");
  runOTA(customUrl);
}

// ============================================================================
// ADVANCED EMBEDDED DASHBOARD (PROGMEM)
// ============================================================================
const char DASHBOARD_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>SmartHub Unified Controller</title>
  <style>
    :root {
      --bg: #090d16; --surface: #131b2e; --surface-border: #202b42;
      --text: #e2e8f0; --muted: #798da3; --accent: #10b981;
      --accent-hover: #059669; --danger: #ef4444; --blue: #3b82f6;
    }
    * { box-sizing: border-box; margin: 0; padding: 0; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; }
    body { background: var(--bg); color: var(--text); padding: 16px; display: flex; justify-content: center; }
    .container { width: 100%; max-width: 540px; display: flex; flex-direction: column; gap: 14px; }
    .card { background: var(--surface); border: 1px solid var(--surface-border); border-radius: 12px; padding: 18px; box-shadow: 0 4px 20px rgba(0,0,0,0.4); }
    h2 { font-size: 18px; font-weight: 700; display: flex; justify-content: space-between; align-items: center; margin-bottom: 12px; }
    h3 { font-size: 14px; font-weight: 600; text-transform: uppercase; color: var(--muted); margin-bottom: 10px; letter-spacing: 0.5px; }
    .tabs { display: flex; gap: 6px; margin-bottom: 12px; background: var(--bg); padding: 4px; border-radius: 8px; border: 1px solid var(--surface-border); }
    .tab { flex: 1; padding: 8px; text-align: center; font-size: 12px; font-weight: 600; cursor: pointer; border-radius: 6px; color: var(--muted); border: none; background: transparent; }
    .tab.active { background: var(--surface); color: var(--text); border: 1px solid var(--surface-border); }
    .tab-content { display: none; }
    .tab-content.active { display: block; }
    .grid { display: grid; grid-template-columns: repeat(2, 1fr); gap: 10px; margin-bottom: 10px; }
    .metric { background: var(--bg); border: 1px solid var(--surface-border); padding: 12px; border-radius: 8px; }
    .metric-label { font-size: 11px; color: var(--muted); text-transform: uppercase; margin-bottom: 4px; }
    .metric-val { font-size: 15px; font-family: monospace; font-weight: 700; color: var(--blue); }
    .btn-group { display: flex; gap: 8px; margin-top: 8px; }
    button { flex: 1; padding: 10px; border: none; border-radius: 6px; font-weight: 600; cursor: pointer; color: #fff; transition: 0.2s; font-size: 13px; }
    button:active { filter: brightness(0.85); }
    .btn-green { background: var(--accent); }
    .btn-red { background: var(--danger); }
    .btn-blue { background: var(--blue); }
    input[type="text"], input[type="password"], select { width: 100%; padding: 10px; background: var(--bg); border: 1px solid var(--surface-border); border-radius: 6px; color: #fff; font-size: 13px; margin-bottom: 8px; }
    input[type="range"] { width: 100%; height: 6px; background: var(--bg); border-radius: 4px; outline: none; margin: 12px 0; }
    #log { font-family: monospace; font-size: 11px; color: var(--muted); background: var(--bg); padding: 10px; border-radius: 6px; min-height: 24px; border: 1px solid var(--surface-border); word-break: break-all; margin-top: 8px; }
  </style>
</head>
<body>
  <div class="container">
    <div class="card">
      <h2>SmartHub Core <span style="font-size:12px; background:var(--surface-border); padding:3px 8px; border-radius:4px; font-weight:normal;" id="v-pill">v1.1.0</span></h2>
      <div class="tabs">
        <button class="tab active" onclick="switchTab('control')">Controls</button>
        <button class="tab" onclick="switchTab('mesh')">Mesh & Slave</button>
        <button class="tab" onclick="switchTab('wifi')">Wi-Fi Setup</button>
        <button class="tab" onclick="switchTab('system')">OTA Engine</button>
      </div>

      <!-- TAB 1: CONTROLS -->
      <div id="tab-control" class="tab-content active">
        <div class="grid">
          <div class="metric"><div class="metric-label">Signal (RSSI)</div><div class="metric-val" id="rssi-val">-0 dBm</div></div>
          <div class="metric"><div class="metric-label">Heap Memory</div><div class="metric-val" id="heap-val">0 KB</div></div>
        </div>
        <h3>Relay 1 (Arduino Uno)</h3>
        <div class="btn-group">
          <button class="btn-green" onclick="sendRelay('1','on')">Turn ON</button>
          <button class="btn-red" onclick="sendRelay('1','off')">Turn OFF</button>
        </div>
        <h3 style="margin-top:14px;">Relay 2 (Arduino Uno)</h3>
        <div class="btn-group">
          <button class="btn-green" onclick="sendRelay('2','on')">Turn ON</button>
          <button class="btn-red" onclick="sendRelay('2','off')">Turn OFF</button>
        </div>
        <h3 style="margin-top:14px;">Slave PWM Regulation: <span id="pwm-readout">0</span></h3>
        <input type="range" min="0" max="255" value="0" id="pwm-slider" oninput="document.getElementById('pwm-readout').innerText = this.value" onchange="sendPWM(this.value)">
      </div>

      <!-- TAB 2: MESH & SLAVE -->
      <div id="tab-mesh" class="tab-content">
        <h3>Connected Stations</h3>
        <div class="grid">
          <div class="metric"><div class="metric-label">Mesh Children</div><div class="metric-val" id="clients-val">0 Nodes</div></div>
          <div class="metric"><div class="metric-label">Active Slave IP</div><div class="metric-val" id="slave-readout">192.168.4.20</div></div>
        </div>
        <h3>Reconfigure Slave Target</h3>
        <input type="text" id="slave-ip-input" placeholder="e.g. 192.168.4.20">
        <button class="btn-blue" style="width:100%;" onclick="updateSlaveIP()">Save Slave Route to EEPROM</button>
      </div>

      <!-- TAB 3: WI-FI SELECTOR -->
      <div id="tab-wifi" class="tab-content">
        <h3>Auto-Connect Status</h3>
        <div class="grid">
          <div class="metric"><div class="metric-label">Active Station</div><div class="metric-val" id="ssid-val">Scanning...</div></div>
          <div class="metric"><div class="metric-label">Assigned IP</div><div class="metric-val" id="ip-val">0.0.0.0</div></div>
        </div>
        <h3>Scan Nearby APs</h3>
        <button class="btn-blue" style="width:100%; margin-bottom:8px;" onclick="scanNetworks()">Discover Networks</button>
        <select id="network-dropdown" onchange="document.getElementById('wifi-ssid').value = this.value">
          <option value="">Select a scanned network...</option>
        </select>
        <input type="text" id="wifi-ssid" placeholder="Network SSID">
        <input type="password" id="wifi-pass" placeholder="Network Password">
        <button class="btn-green" style="width:100%;" onclick="saveWiFi()">Commit to EEPROM & Auto-Scan</button>
      </div>

      <!-- TAB 4: SYSTEM & OTA -->
      <div id="tab-system" class="tab-content">
        <h3>Firmware Information</h3>
        <div class="metric" style="margin-bottom:12px;">
          <div class="metric-label">Core Uptime</div>
          <div class="metric-val" id="uptime-val">0 s</div>
        </div>
        <h3>Automated GitHub Pull</h3>
        <button class="btn-blue" style="width:100%; margin-bottom:12px;" onclick="triggerOTA('')">Poll Latest GitHub Release</button>
        <h3>Direct Binary URL Push</h3>
        <input type="text" id="custom-ota-url" placeholder="https://domain.com/firmware.bin">
        <button class="btn-green" style="width:100%;" onclick="triggerOTA(document.getElementById('custom-ota-url').value)">Deploy Custom Binary</button>
      </div>

      <div id="log">Console ready.</div>
    </div>
  </div>

  <script>
    function switchTab(name) {
      document.querySelectorAll('.tab').forEach(t => t.classList.remove('active'));
      document.querySelectorAll('.tab-content').forEach(c => c.classList.remove('active'));
      event.target.classList.add('active');
      document.getElementById('tab-' + name).classList.add('active');
    }

    function getBase() {
      if (window.location.hostname && !window.location.hostname.includes("github.io")) {
        return window.location.origin;
      }
      return "http://192.168.4.1";
    }

    function log(msg, color) {
      var l = document.getElementById('log');
      l.innerText = msg;
      l.style.color = color || '#798da3';
    }

    function updateTelemetry() {
      fetch(getBase() + '/api/status', { mode: 'cors' })
        .then(r => r.json())
        .then(d => {
          document.getElementById('v-pill').innerText = 'v' + d.version;
          document.getElementById('rssi-val').innerText = d.rssi + ' dBm';
          document.getElementById('heap-val').innerText = Math.round(d.free_heap / 1024) + ' KB';
          document.getElementById('clients-val').innerText = d.mesh_clients + ' Nodes';
          document.getElementById('slave-readout').innerText = d.slave_ip;
          document.getElementById('ssid-val').innerText = d.ssid;
          document.getElementById('ip-val').innerText = d.ip;
          document.getElementById('uptime-val').innerText = Math.round(d.uptime_ms / 1000) + ' s';
        })
        .catch(() => log('Telemetry connection timeout.', '#ef4444'));
    }

    function sendRelay(ch, act) {
      log('Dispatching Relay ' + ch + ' -> ' + act + '...');
      fetch(getBase() + '/api/relay?ch=' + ch + '&state=' + act, { mode: 'cors' })
        .then(r => r.text())
        .then(t => log('Slave Output: ' + t, '#10b981'))
        .catch(() => log('Relay dispatch error.', '#ef4444'));
    }

    function sendPWM(v) {
      fetch(getBase() + '/api/pwm?val=' + v, { mode: 'cors' })
        .catch(() => log('PWM write failed.', '#ef4444'));
    }

    function updateSlaveIP() {
      var ip = document.getElementById('slave-ip-input').value;
      fetch(getBase() + '/api/config-slave?ip=' + encodeURIComponent(ip), { method: 'POST', mode: 'cors' })
        .then(r => r.text())
        .then(() => log('Slave routing updated: ' + ip, '#10b981'));
    }

    function scanNetworks() {
      log('Probing RF spectrum...');
      fetch(getBase() + '/api/scan', { mode: 'cors' })
        .then(r => r.json())
        .then(list => {
          var dd = document.getElementById('network-dropdown');
          dd.innerHTML = '<option value="">Select a scanned network...</option>';
          list.forEach(n => {
            var opt = document.createElement('option');
            opt.value = n.ssid;
            opt.innerText = n.ssid + ' (' + n.rssi + ' dBm)';
            dd.appendChild(opt);
          });
          log('Discovered ' + list.length + ' Wi-Fi APs.', '#10b981');
        });
    }

    function saveWiFi() {
      var s = encodeURIComponent(document.getElementById('wifi-ssid').value);
      var p = encodeURIComponent(document.getElementById('wifi-pass').value);
      fetch(getBase() + '/api/save-wifi?ssid=' + s + '&pass=' + p, { method: 'POST', mode: 'cors' })
        .then(() => log('Network profiles saved. Triggering multi-scan reboot...', '#10b981'));
    }

    function triggerOTA(u) {
      log('Instructing NodeMCU to fetch binary...');
      fetch(getBase() + '/api/trigger-ota' + (u ? '?url=' + encodeURIComponent(u) : ''), { mode: 'cors' })
        .then(r => r.text())
        .then(t => log(t, '#10b981'));
    }

    updateTelemetry();
    setInterval(updateTelemetry, 4000);
  </script>
</body>
</html>)rawliteral";

void handleRoot() {
  applyHeaders();
  server.send_P(200, "text/html", DASHBOARD_HTML);
}

// ============================================================================
// SYSTEM ENTRY & LOOP
// ============================================================================
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=======================================================");
  Serial.printf(" SmartHub Master Gateway - Firmware %s\n", HARDWARE_VERSION.c_str());
  Serial.println("=======================================================");

  EEPROM.begin(EEPROM_SIZE);
  loadEEPROMConfig();
  connectToBestAvailableNetwork();

  // Root UI
  server.on("/", HTTP_GET, handleRoot);

  // REST API Routes
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/status", HTTP_OPTIONS, handleOptions);

  server.on("/api/relay", HTTP_GET, handleRelay);
  server.on("/api/relay", HTTP_OPTIONS, handleOptions);

  server.on("/api/pwm", HTTP_GET, handlePWM);
  server.on("/api/pwm", HTTP_OPTIONS, handleOptions);

  server.on("/api/config-slave", HTTP_POST, handleConfigSlave);
  server.on("/api/config-slave", HTTP_OPTIONS, handleOptions);

  server.on("/api/scan", HTTP_GET, handleScan);
  server.on("/api/scan", HTTP_OPTIONS, handleOptions);

  server.on("/api/save-wifi", HTTP_POST, handleSaveWiFi);
  server.on("/api/save-wifi", HTTP_OPTIONS, handleOptions);

  server.on("/api/trigger-ota", HTTP_GET, handleTriggerOTA);
  server.on("/api/trigger-ota", HTTP_OPTIONS, handleOptions);

  server.onNotFound([]() {
    if (server.method() == HTTP_OPTIONS) {
      handleOptions();
    } else {
      applyHeaders();
      server.send(404, "text/plain", "Endpoint Not Found");
    }
  });

  server.begin();
  Serial.println("[HTTP] API Engine & Unified Dashboard online on port 80.");
}

void loop() {
  server.handleClient();

  if (millis() - lastOTACheck > otaIntervalMs) {
    lastOTACheck = millis();
    runOTA();
  }
}
