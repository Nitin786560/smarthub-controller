#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266httpUpdate.h>
#include <WiFiClientSecure.h>
#include <EEPROM.h>

const String HARDWARE_VERSION = "1.0.0";
const char* GITHUB_REPO       = "Nitin786560/smarthub-controller";

const String OTA_BIN_URL = "http://github.com/" + String(GITHUB_REPO) + "/releases/latest/download/firmware.bin";
const String OTA_MD5_URL = "http://github.com/" + String(GITHUB_REPO) + "/releases/latest/download/firmware.bin.md5";

const char* DEFAULT_HOME_SSID  = "YOUR_HOME_WIFI";
const char* DEFAULT_HOME_PASS  = "YOUR_HOME_PASS";
const char* FALLBACK_REDMI_SSID = "Redmi";
const char* FALLBACK_REDMI_PASS = "1234567890";
const char* AP_SSID            = "SmartHub-NodeMCU";
const char* AP_PASS            = "1234567890";

#define EEPROM_SIZE       128
#define EEPROM_FLAG_ADDR  0
#define EEPROM_SSID_ADDR  1
#define EEPROM_PASS_ADDR  33
#define EEPROM_MAGIC_BYTE 0x4A

ESP8266WebServer server(80);
unsigned long lastOTACheck = 0;
const unsigned long OTA_CHECK_INTERVAL = 60000;

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

bool tryConnect(const char* ssid, const char* pass, int timeoutSec = 10) {
  if (strlen(ssid) == 0) return false;
  Serial.printf("[WiFi] Attempting handshake: %s ...\n", ssid);
  WiFi.begin(ssid, pass);

  int elapsed = 0;
  while (WiFi.status() != WL_CONNECTED && elapsed < (timeoutSec * 2)) {
    delay(500);
    Serial.print(".");
    elapsed++;
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[WiFi] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
    return true;
  }
  return false;
}

void initNetwork() {
  WiFi.mode(WIFI_AP_STA);

  char eepromSSID[32];
  char eepromPASS[32];
  readEEPROMCreds(eepromSSID, eepromPASS);

  if (!tryConnect(DEFAULT_HOME_SSID, DEFAULT_HOME_PASS, 8)) {
    if (!tryConnect(FALLBACK_REDMI_SSID, FALLBACK_REDMI_PASS, 8)) {
      if (!tryConnect(eepromSSID, eepromPASS, 8)) {
        Serial.println("[WiFi] Running in Standalone AP mode.");
      }
    }
  }

  IPAddress apIP(192, 168, 4, 1);
  IPAddress netMsk(255, 255, 255, 0);
  WiFi.softAPConfig(apIP, apIP, netMsk);
  WiFi.softAP(AP_SSID, AP_PASS);
  Serial.printf("[WiFi] AP Active: %s (IP: 192.168.4.1)\n", AP_SSID);
}

void runOTA() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[OTA] Skipped: No internet connectivity.");
    return;
  }

  Serial.println("[OTA] Querying release assets...");
  WiFiClient client;
  ESPhttpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  ESPhttpUpdate.rebootOnUpdate(true);

  t_httpUpdate_return ret = ESPhttpUpdate.update(client, OTA_BIN_URL);

  switch (ret) {
    case HTTP_UPDATE_FAILED:
      Serial.printf("[OTA] Failed (%d): %s\n", ESPhttpUpdate.getLastError(), ESPhttpUpdate.getLastErrorString().c_str());
      break;
    case HTTP_UPDATE_NO_UPDATES:
      Serial.println("[OTA] Already up to date.");
      break;
    case HTTP_UPDATE_OK:
      Serial.println("[OTA] Success! Flashing and rebooting...");
      break;
  }
}

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

void handleRoot() {
  String html = "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
                "<title>SmartHub Controller</title><style>"
                "body{font-family:sans-serif;background:#0d1117;color:#c9d1d9;padding:20px;text-align:center;}"
                ".card{background:#161b22;padding:20px;border-radius:10px;display:inline-block;max-width:400px;width:100%;}"
                "button{background:#238636;color:#fff;border:none;padding:10px 20px;border-radius:6px;cursor:pointer;font-weight:bold;margin:5px;}"
                "input{width:90%;padding:8px;margin:8px 0;background:#0d1117;border:1px solid #30363d;color:#fff;border-radius:4px;}"
                "</style></head><body>"
                "<div class='card'>"
                "<h2>SmartHub Master</h2>"
                "<p>Firmware Version: <b>" + HARDWARE_VERSION + "</b></p>"
                "<p>Node IP: <b>" + (WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : WiFi.softAPIP().toString()) + "</b></p>"
                "<hr style='border:1px solid #30363d;'>"
                "<h3>Slave Command Router</h3>"
                "<button onclick=\"fetch('/api/relay?action=on')\">Relay ON</button>"
                "<button style='background:#da3633;' onclick=\"fetch('/api/relay?action=off')\">Relay OFF</button>"
                "<hr style='border:1px solid #30363d;'>"
                "<h3>Save Wi-Fi Profile</h3>"
                "<form action='/api/save-wifi' method='POST'>"
                "<input type='text' name='ssid' placeholder='SSID' required><br>"
                "<input type='password' name='pass' placeholder='Password' required><br>"
                "<button type='submit'>Save & Reconnect</button>"
                "</form>"
                "<hr style='border:1px solid #30363d;'>"
                "<button style='background:#1f6feb;' onclick=\"fetch('/api/trigger-ota')\">Check for GitHub OTA</button>"
                "</div></body></html>";
  server.send(200, "text/html", html);
}

void handleSaveWiFi() {
  if (server.hasArg("ssid") && server.hasArg("pass")) {
    writeEEPROMCreds(server.arg("ssid"), server.arg("pass"));
    server.send(200, "text/plain", "Credentials stored. Reconnecting...");
    delay(1000);
    ESP.restart();
  } else {
    server.send(400, "text/plain", "Missing SSID or Password");
  }
}

void handleRelayRoute() {
  String action = server.hasArg("action") ? server.arg("action") : "status";
  String slaveIp = "192.168.4.20";
  String response = forwardCommandToSlave(slaveIp, "/relay?state=" + action);
  server.send(200, "application/json", response);
}

void handleTriggerOTA() {
  server.send(200, "text/plain", "Triggering GitHub pull verification...");
  runOTA();
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n--- [SmartHub Master Controller Booting] ---");
  Serial.printf("Current Version: %s\n", HARDWARE_VERSION.c_str());

  EEPROM.begin(EEPROM_SIZE);
  initNetwork();

  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/save-wifi", HTTP_POST, handleSaveWiFi);
  server.on("/api/relay", HTTP_GET, handleRelayRoute);
  server.on("/api/trigger-ota", HTTP_GET, handleTriggerOTA);
  server.begin();
  Serial.println("[HTTP] Master web server listening on port 80.");
}

void loop() {
  server.handleClient();

  if (millis() - lastOTACheck > OTA_CHECK_INTERVAL) {
    lastOTACheck = millis();
    runOTA();
  }
}
