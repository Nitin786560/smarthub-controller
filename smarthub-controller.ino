#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266httpUpdate.h> 
#include <EEPROM.h>

ESP8266WebServer server(80);

// ====================================================================
// HARDCODED PROFILE DEFINITIONS & GLOBAL VERSION REGISTRY
// ====================================================================
const String HARDWARE_VERSION = "1.0.0"; 

const char* default_home_ssid = "YOUR_HOME_WIFI_NAME";  
const char* default_home_pass = "YOUR_HOME_PASSWORD";   
const char* fallback_redmi_ssid = "Redmi";
const char* fallback_redmi_pass = "1234567890";

const char* global_github_bin_url = "http://githubusercontent.com";

#define ADDR_CUSTOM_SSID 0
#define ADDR_CUSTOM_PASS 64
#define ADDR_MASTER_SSID 128
#define ADDR_MASTER_PASS 192

String custom_ssid = ""; String custom_pass = "";
String fallback_ap_ssid = "SmartHub-NodeMCU"; String fallback_ap_pass = "1234567890";

String active_net_status = "Scanning Networks...";
unsigned long lastCloudPoll = 0;
const long cloudPollInterval = 60000; 

// Hardware Output Mapping Tracker
const int appliancePin = 5; // NodeMCU Pin D1 mapped safely to dynamic register values
bool currentPinState = LOW;

// --- Storage Read/Write Blocks ---
void writeStringToEEPROM(int addr, String str) {
  byte len = str.length(); EEPROM.write(addr, len);
  for (int i = 0; i < len; i++) EEPROM.write(addr + 1 + i, str[i]);
  EEPROM.commit();
}
String readStringFromEEPROM(int addr) {
  byte len = EEPROM.read(addr); if (len == 0 || len > 63) return ""; 
  char data[len + 1]; for (int i = 0; i < len; i++) data[i] = EEPROM.read(addr + 1 + i);
  data[len] = '\0'; return String(data);
}

void executeSmartNetworkHandshake() {
  WiFi.disconnect(); delay(100);
  int n = WiFi.scanNetworks();
  bool homeSeen = false, redmiSeen = false, customSeen = false;
  for (int i = 0; i < n; ++i) {
    if (WiFi.SSID(i) == String(default_home_ssid))  homeSeen = true;
    if (WiFi.SSID(i) == String(redmi_ssid))         redmiSeen = true;
    if (custom_ssid.length() > 0 && WiFi.SSID(i) == custom_ssid) customSeen = true;
  }
  if (homeSeen)        WiFi.begin(default_home_ssid, default_home_pass);
  else if (redmiSeen)  WiFi.begin(fallback_redmi_ssid, fallback_redmi_pass);
  else if (customSeen) WiFi.begin(custom_ssid.c_str(), custom_pass.c_str());
  else                 WiFi.begin(default_home_ssid, default_home_pass);

  unsigned long startWait = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startWait < 10000) { delay(400); }
  active_net_status = (WiFi.status() == WL_CONNECTED) ? WiFi.SSID() : "Standalone AP Active (Offline)";
}

void fetchGlobalGitHubUpdate() {
  if (WiFi.status() == WL_CONNECTED) {
    WiFiClientSecure client;
    client.setInsecure(); 
    t_httpUpdate_return ret = ESPhttpUpdate.update(client, global_github_bin_url, HARDWARE_VERSION);
  }
}

// ====================================================================
// CRITICAL SYNCHRONIZATION HOOK: Captures the clicks from your GitHub website
// ====================================================================
void handleWirelessCommandAPI() {
  if (server.hasArg("s") && server.hasArg("p")) {
    int targetState = server.arg("p").toInt(); // Extracts '1' for ON, '0' for OFF
    
    currentPinState = (targetState == 1) ? HIGH : LOW;
    digitalWrite(appliancePin, currentPinState);
    
    // Pass execution token tracking packets immediately to the hardware line
    if(currentPinState == HIGH) {
      Serial.println("SET:8:1");
    } else {
      Serial.println("SET:8:0");
    }
    
    server.send(200, "text/plain", "OK");
  } else {
    server.send(400, "text/plain", "Bad Submission");
  }
}

void handleLandingPage() {
  String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<title>SmartHub Gateway Panel</title><style>body{font-family:system-ui,sans-serif; background:#0f172a; color:#f8fafc; margin:0; padding:16px; display:flex; justify-content:center; align-items:center; min-height:100vh;} .wrap{width:100%; max-width:420px; text-align:center;} h1{color:#38bdf8;} .card{background:#1e293b; border:1px solid #334155; border-radius:14px; padding:20px; text-align:left; margin-bottom:16px;} .row{display:flex; justify-content:space-between; margin-bottom:10px; font-size:14px; border-bottom:1px solid #1e293b; padding-bottom:6px;} .badge{padding:2px 8px; border-radius:4px; font-weight:600; background:#10b981;} .btn{background:#0284c7; color:#fff; border:none; padding:12px; font-weight:600; width:100%; border-radius:8px; cursor:pointer; text-decoration:none; display:block; box-sizing:border-box;}</style></head><body>";
  html += "<div class='wrap'><h1>Local Interface Portal</h1><div class='card'>";
  html += "<div class='row'><span>Active Gateway:</span> <span class='badge'>" + active_net_status + "</span></div>";
  html += "<div class='row'><span>Local Node Address:</span> <span>" + WiFi.localIP().toString() + "</span></div>";
  html += "<div class='row'><span>Hardware Pin 8 Status:</span> <span>" + String(currentPinState ? "HIGH (ON)" : "LOW (OFF)") + "</span></div>";
  html += "<div class='row'><span>Running Firmware Build:</span> <span>v" + HARDWARE_VERSION + "</span></div></div>";
  html += "<form action='/reboot' method='POST'><button type='submit' class='btn' style='background:#dc2626;'>Restart NodeMCU</button></form></div></body></html>";
  server.send(200, "text/html", html);
}

void handleReboot() { server.send(200, "text/html", "<h2>Rebooting Core...</h2>"); delay(1000); ESP.restart(); }

void setup() {
  Serial.begin(115200); EEPROM.begin(512);
  pinMode(appliancePin, OUTPUT);
  digitalWrite(appliancePin, currentPinState);

  String stored_master_ssid = readStringFromEEPROM(ADDR_MASTER_SSID);
  String stored_master_pass = readStringFromEEPROM(ADDR_MASTER_PASS);
  if(stored_master_ssid.length() > 0 && stored_master_ssid.length() < 32) {
    fallback_ap_ssid = stored_master_ssid; fallback_ap_pass = stored_master_pass;
  }
  custom_ssid = readStringFromEEPROM(ADDR_CUSTOM_SSID);
  custom_pass = readStringFromEEPROM(ADDR_CUSTOM_PASS);

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(fallback_ap_ssid.c_str(), fallback_ap_pass.c_str());

  executeSmartNetworkHandshake();

  server.on("/", handleLandingPage);
  server.on("/save-wifi", handleWirelessCommandAPI); // Direct API anchor integration link
  server.on("/reboot", handleReboot);
  server.begin();
}

void loop() {
  server.handleClient();
  
  unsigned long currentMillis = millis();
  if (currentMillis - lastCloudPoll >= cloudPollInterval) {
    lastCloudPoll = currentMillis;
    fetchGlobalGitHubUpdate();
  }
}
