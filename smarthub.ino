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

#define EEPROM_SIZE        256
#define ADDR_MAGIC         0
#define ADDR_SSID          1
#define ADDR_PASS          33
#define ADDR_SLAVE_IP      65
#define ADDR_OTA_INTERVAL  85
#define EEPROM_MAGIC_BYTE  0x5B

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
// EEPROM CONFIGURATION STORAGE
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
// MULTI-NETWORK SCANNER & AUTO-CONNECT
// ============================================================================
void connectToBestAvailableNetwork() {
  WiFi.mode(WIFI_AP_STA);
  Serial.println("[WiFi] Scanning local spectrum for networks...");

  char eepromSSID[32] = {0};
  char eepromPASS[32] = {0};
  if (EEPROM.read(ADDR_MAGIC) == EEPROM_MAGIC_BYTE) {
    for (int i = 0; i < 32; ++i) eepromSSID[i] = char(EEPROM.read(ADDR_SSID + i));
    for (int i = 0; i < 32; ++i) eepromPASS[i] = char(EEPROM.read(ADDR_PASS + i));
  }

  int scanResults = WiFi.scanNetworks();
  bool connected = false;

  if (scanResults > 0) {
    for (int i = 0; i < scanResults; ++i) {
      String ssidFound = WiFi.SSID(i);
      if (ssidFound == DEFAULT_HOME_SSID) {
        WiFi.begin(DEFAULT_HOME_SSID, DEFAULT_HOME_PASS);
        connected = (WiFi.waitForConnectResult() == WL_CONNECTED);
        if (connected) break;
      } else if (ssidFound == FALLBACK_REDMI_SSID) {
        WiFi.begin(FALLBACK_REDMI_SSID, FALLBACK_REDMI_PASS);
        connected = (WiFi.waitForConnectResult() == WL_CONNECTED);
        if (connected) break;
      } else if (strlen(eepromSSID) > 0 && ssidFound == String(eepromSSID)) {
        WiFi.begin(eepromSSID, eepromPASS);
        connected = (WiFi.waitForConnectResult() == WL_CONNECTED);
        if (connected) break;
      }
    }
  }

  WiFi.scanDelete();

  if (connected) {
    Serial.printf("[WiFi] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("[WiFi] No known networks reachable. Fallback to AP.");
  }

  IPAddress apIP(192, 168, 4, 1);
  IPAddress netMsk(255, 255, 255, 0);
  WiFi.softAPConfig(apIP, apIP, netMsk);
  WiFi.softAP(AP_SSID, AP_PASS);
  Serial.printf("[WiFi] AP Ready: %s (IP: 192.168.4.1)\n", AP_SSID);
}

// ============================================================================
// SECURE GITHUB OTA RUNNER (WITH SSL REDIRECT TRAVERSAL)
// ============================================================================
void runOTA(String customUrl = "") {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[OTA] Skipped: Not connected to external network.");
    return;
  }

  String targetUrl = (customUrl.length() > 0) ? customUrl : 
    ("https://github.com/" + String(GITHUB_REPO) + "/releases/latest/download/firmware.bin");

  Serial.printf("[OTA] Fetching target: %s\n", targetUrl.c_str());

  WiFiClientSecure client;
  client.setInsecure();
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
      Serial.println("[OTA] Update flashed. Rebooting device...");
      break;
  }
}

// ============================================================================
// ARDUINO SLAVE COMMAND ROUTER
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
  String payload = (httpCode > 0) ? http.getString() : "{\"status\":\"unreachable\"}";
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
    server.send(400, "text/plain", "Missing val parameter");
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
    server.send(200, "text/plain", "Credentials saved. Rebooting...");
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
    json += "{\"ssid\":\"" + WiFi.SSID(i) + "\",\"rssi\":" + String(WiFi.RSSI(i)) + "}";
  }
  json += "]";
  WiFi.scanDelete();
  server.send(200, "application/json", json);
}

void handleTriggerOTA() {
  applyHeaders();
  String customUrl = server.hasArg("url") ? server.arg("url") : "";
  server.send(200, "text/plain", "OTA check launched.");
  runOTA(customUrl);
}

// ============================================================================
// EMBEDDED DASHBOARD UI (FLASH-BUFFERED VIA PROGMEM)
// ============================================================================
const char DASHBOARD_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>SmartHub Controller</title>
  <style>
    :root { --bg: #090d16; --surface: #131b2e; --border: #202b42; --text: #e2e8f0; --muted: #798da3; --accent: #10b981; --danger: #ef4444; --blue: #3b82f6; }
    * { box-sizing: border-box; margin: 0; padding: 0; font-family: sans-serif; }
    body { background: var(--bg); color: var(--text); padding: 16px; display: flex; justify-content: center; }
    .container { width: 100%; max-width: 500px; display: flex; flex-direction: column; gap: 14px; }
    .card { background: var(--surface); border: 1px solid var(--border); border-radius: 12px; padding: 18px; }
    h2 { font-size: 18px; display: flex; justify-content: space-between; align-items: center; margin-bottom: 12px; }
    h3 { font-size: 13px; text-transform: uppercase; color: var(--muted); margin-bottom: 10px; }
    .grid { display: grid; grid-template-columns: repeat(2, 1fr); gap: 10px; margin-bottom: 10px; }
    .metric { background: var(--bg); border: 1px solid var(--border); padding: 10px; border-radius: 8px; }
    .metric-label { font-size: 11px; color: var(--muted); }
    .metric-val { font-size: 14px; font-family: monospace; font-weight: bold; color: var(--blue); }
    .btn-group { display: flex; gap: 8px; margin-top: 8px; }
    button { flex: 1; padding: 10px; border: none; border-radius: 6px; font-weight: bold; cursor: pointer; color: #fff; }
    .btn-green { background: var(--accent); }
    .btn-red { background: var(--danger); }
    .btn-blue { background: var(--blue); width: 100%; }
    input, select { width: 100%; padding: 10px; background: var(--bg); border: 1px solid var(--border); border-radius: 6px; color: #fff; margin-bottom: 8px; }
    #log { font-family: monospace; font-size: 11px; color: var(--muted); background: var(--bg); padding: 10px; border-radius: 6px; margin-top: 8px; min-height: 20px; }
  </style>
</head>
<body>
  <div class="container">
    <div class="card">
      <h2>SmartHub Core <span style="font-size:12px; background:var(--border); padding:2px 8px; border-radius:4px;" id="v-pill">...</span></h2>
      <div class="grid">
        <div class="metric"><div class="metric-label">Signal</div><div class="metric-val" id="rssi-val">--</div></div>
        <div class="metric"><div class="metric-label">Mesh Nodes</div><div class="metric-val" id="clients-val">--</div></div>
        <div class="metric"><div class="metric-label">Active SSID</div><div class="metric-val" id="ssid-val">--</div></div>
        <div class="metric"><div class="metric-label">Station IP</div><div class="metric-val" id="ip-val">--</div></div>
      </div>
    </div>

    <div class="card">
      <h3>Relay 1 Control (Arduino Uno)</h3>
      <div class="btn-group">
        <button class="btn-green" onclick="sendRelay('1','on')">Turn ON</button>
        <button class="btn-red" onclick="sendRelay('1','off')">Turn OFF</button>
      </div>
      <h3 style="margin-top:14px;">Slave PWM Duty (0-255)</h3>
      <input type="range" min="0" max="255" value="0" onchange="sendPWM(this.value)" style="margin-bottom:4px;">
    </div>

    <div class="card">
      <h3>Arduino Slave IP Target</h3>
      <input type="text" id="slave-ip" placeholder="192.168.4.20">
      <button class="btn-blue" onclick="updateSlaveIP()">Save Slave IP</button>
    </div>

    <div class="card">
      <h3>Wi-Fi Profile Manager</h3>
      <button class="btn-blue" style="margin-bottom:8px;" onclick="scanNetworks()">Scan Networks</button>
      <select id="network-dropdown" onchange="document.getElementById('wifi-ssid').value = this.value">
        <option value="">Select a visible network...</option>
      </select>
      <input type="text" id="wifi-ssid" placeholder="SSID">
      <input type="password" id="wifi-pass" placeholder="Password">
      <button class="btn-green" style="width:100%;" onclick="saveWiFi()">Commit & Reboot</button>
    </div>

    <div class="card">
      <h3>Worldwide OTA Update</h3>
      <button class="btn-blue" onclick="triggerOTA()">Poll GitHub Release</button>
      <div id="log">Dashboard ready.</div>
    </div>
  </div>

  <script>
    function getBase() {
      return (window.location.hostname && !window.location.hostname.includes("github.io")) ? window.location.origin : "http://192.168.4.1";
    }

    function log(msg) {
      document.getElementById('log').innerText = msg;
    }

    function updateTelemetry() {
      fetch(getBase() + '/api/status', { mode: 'cors' })
        .then(function(r) { return r.json(); })
        .then(function(d) {
          document.getElementById('v-pill').innerText = 'v' + d.version;
          document.getElementById('rssi-val').innerText = d.rssi + ' dBm';
          document.getElementById('clients-val').innerText = d.mesh_clients;
          document.getElementById('ssid-val').innerText = d.ssid;
          document.getElementById('ip-val').innerText = d.ip;
          document.getElementById('slave-ip').value = d.slave_ip;
        })
        .catch(function() { log('Connection timeout to ' + getBase()); });
    }

    function sendRelay(ch, act) {
      fetch(getBase() + '/api/relay?ch=' + ch + '&state=' + act, { mode: 'cors' })
        .then(function(r) { return r.text(); })
        .then(function(t) { log('Slave: ' + t); });
    }

    function sendPWM(v) {
      fetch(getBase() + '/api/pwm?val=' + v, { mode: 'cors' });
    }

    function updateSlaveIP() {
      var ip = document.getElementById('slave-ip').value;
      fetch(getBase() + '/api/config-slave?ip=' + encodeURIComponent(ip), { method: 'POST', mode: 'cors' })
        .then(function() { log('Updated target slave IP.'); });
    }

    function scanNetworks() {
      log('Scanning...');
      fetch(getBase() + '/api/scan', { mode: 'cors' })
        .then(function(r) { return r.json(); })
        .then(function(list) {
          var dd = document.getElementById('network-dropdown');
          dd.innerHTML = '<option value="">Select a visible network...</option>';
          list.forEach(function(n) {
            var opt = document.createElement('option');
            opt.value = n.ssid;
            opt.innerText = n.ssid + ' (' + n.rssi + ' dBm)';
            dd.appendChild(opt);
          });
          log('Found ' + list.length + ' networks.');
        });
    }

    function saveWiFi() {
      var s = encodeURIComponent(document.getElementById('wifi-ssid').value);
      var p = encodeURIComponent(document.getElementById('wifi-pass').value);
      fetch(getBase() + '/api/save-wifi?ssid=' + s + '&pass=' + p, { method: 'POST', mode: 'cors' })
        .then(function() { log('Saved credentials. Node rebooting...'); });
    }

    function triggerOTA() {
      log('Checking GitHub releases...');
      fetch(getBase() + '/api/trigger-ota', { mode: 'cors' })
        .then(function(r) { return r.text(); })
        .then(function(t) { log(t); });
    }

    updateTelemetry();
    setInterval(updateTelemetry, 4000);
  </script>
</body>
</html>
)HTML";

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
  Serial.println("\n--- [SmartHub Master Controller Booting] ---");

  EEPROM.begin(EEPROM_SIZE);
  loadEEPROMConfig();
  connectToBestAvailableNetwork();

  server.on("/", HTTP_GET, handleRoot);
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
      server.send(404, "text/plain", "Not Found");
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
