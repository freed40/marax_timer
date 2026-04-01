/**
 * MaraX Timer — ESP32 DevKit V1
 *
 * Verkabelung:
 *   Reed-Modul D0 -> GPIO 18 (VCC 3.3V, GND, A0 unbelegt)
 *   OLED SSD1306 I2C: SDA GPIO 21, SCL GPIO 22
 *   MaraX UART (wie NodeMCU D5/D6): RX GPIO 14, TX GPIO 12
 *
 * Timer: Hochzählen bei Reed-Kontakt (Pumpen aktiv), Stopp und Anzeige des
 * Ergebnisses nach Kontakt-Offen (500ms Entprellung wie im Original).
 *
 * Home Assistant: optional MQTT wie alexander-heimbuch/marax_timer (PubSubClient).
 * Topics: /marax/power, /marax/pump, /marax/hx, /marax/steam, /marax/shot,
 *         /marax/machineheating, /marax/machineheatingboost
 * YAML-Beispiel: https://github.com/alexander-heimbuch/marax_timer
 *
 * Webinterface + lokaler Verlauf: bei gesetztem WIFI_SSID (ohne MQTT möglich).
 * Shots ≥15 s werden mit Zeitstempel (UTC, NTP) und optional HX/Dampf in LittleFS
 * gespeichert. Browser: http://<IP>/ oder http://marax.local/ (mDNS).
 *
 * Firmware-OTA: /update — .bin per Browser hochladen (Arduino: Sketch → Export
 * compiled Binary). OTA_UPDATE_TOKEN im Sketch setzen, sonst deaktiviert.
 *
 * WLAN: Zugangsdaten werden im Flash (NVS) gespeichert. Ohne Eintrag und ohne
 * WIFI_SSID im Sketch startet ein AP „MaraX-Timer“ → http://192.168.4.1/ eintragen.
 * Bei falschem Passwort: Gerät startet denselben Konfig-AP. /wifi = WLAN ändern.
 */

#define REED_PIN 18
#define I2C_SDA 21
#define I2C_SCL 22
#define MARAX_RX 14
#define MARAX_TX 12

#include <Adafruit_SSD1306.h>
#include <Adafruit_GFX.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <Update.h>
#include <time.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <Timer.h>
#include <HardwareSerial.h>
#include <Preferences.h>

// Optional: fest im Sketch (nur Ersteinrichtung / ohne NVS). NVS überschreibt dies.
const char *WIFI_SSID = "";
const char *WIFI_PASSWORD = "";
// Passwort für den Konfigurations-Access-Point (min. 8 Zeichen für WPA2, sonst offenes AP)
const char *CONFIG_AP_PASSWORD = "maraxsetup";
const char *MQTT_BROKER = "";
const uint16_t MQTT_PORT = 1883;
const char *MQTT_USER = "";
const char *MQTT_PASSWORD = "";

// Firmware-Update per Webbrowser (/update). Leer lassen = OTA-Seite gesperrt.
const char *OTA_UPDATE_TOKEN = "";

#define MARAX_TEMP_INVALID (-1000)

Adafruit_SSD1306 display(128, 64, &Wire, -1);
HardwareSerial MaraXSerial(2);
Timer t;

WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);
WebServer server(80);

#define SHOTS_LOG_PATH "/shots.log"
#define SHOTS_LOG_MAX_BYTES 24000

static bool otaUploadSucceeded = false;

Preferences prefs;
String g_wifiStaSsid;
String g_wifiStaPass;
static bool g_configPortalMode = false;
static bool g_webServerStarted = false;

volatile bool reedInterruptPending = false;

void IRAM_ATTR onReedChanged() {
  reedInterruptPending = true;
}

// true/false je nach Reed-Typ (Modul / Schaltlogik); bei falscher Polarität anpassen
bool reedOpenSensor = true;
bool displayOn = true;
int timerCount = 0;
int prevTimerCount = 0;
bool timerStarted = false;
long timerStartMillis = 0;
long timerStopMillis = 0;
long timerDisplayOffMillis = 0;
long serialUpdateMillis = 0;
int pumpInValue = 0;

String machineState;
bool machineHeating = false;
bool machineHeatingBoost = false;
int hxTemperature = MARAX_TEMP_INVALID;
int steamTemperature = MARAX_TEMP_INVALID;

String lastMachineState;
int lastHxTemperature = MARAX_TEMP_INVALID - 1;
int lastSteamTemperature = MARAX_TEMP_INVALID - 1;
long lastTimerStartMillis = -1;
bool lastTimerStarted = false;
bool lastMachineHeating = false;
bool lastMachineHeatingBoost = false;

const byte numChars = 32;
char receivedChars[numChars];
static byte ndx = 0;
char endMarker = '\n';
char rc;

static void loadWifiCredentials() {
  prefs.begin("marax", false);
  g_wifiStaSsid = prefs.getString("ssid", "");
  g_wifiStaPass = prefs.getString("pass", "");
  if (g_wifiStaSsid.isEmpty() && WIFI_SSID[0] != '\0') {
    g_wifiStaSsid = WIFI_SSID;
    g_wifiStaPass = WIFI_PASSWORD;
  }
}

static bool wifiCredentialsConfigured() {
  return g_wifiStaSsid.length() > 0;
}

static bool mqttConfigured() {
  return MQTT_BROKER[0] != '\0' && wifiCredentialsConfigured() &&
         WiFi.status() == WL_CONNECTED;
}

static void startConfigPortal() {
  g_configPortalMode = true;
  WiFi.mode(WIFI_AP);
  if (strlen(CONFIG_AP_PASSWORD) >= 8) {
    WiFi.softAP("MaraX-Timer", CONFIG_AP_PASSWORD);
  } else {
    WiFi.softAP("MaraX-Timer");
  }
  Serial.print("Konfig-AP aktiv. IP: ");
  Serial.println(WiFi.softAPIP());
}

static bool tryConnectSta() {
  if (!wifiCredentialsConfigured()) {
    return false;
  }
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(g_wifiStaSsid.c_str(), g_wifiStaPass.c_str());
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 45000) {
    delay(100);
  }
  return WiFi.status() == WL_CONNECTED;
}

static void syncNetworkTime() {
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  for (int i = 0; i < 25; i++) {
    time_t now = time(nullptr);
    if (now > 1700000000)
      return;
    delay(200);
  }
}

static void trimShotsLogIfNeeded() {
  File f = LittleFS.open(SHOTS_LOG_PATH, "r");
  if (!f || f.size() <= SHOTS_LOG_MAX_BYTES) {
    if (f)
      f.close();
    return;
  }
  String content = f.readString();
  f.close();
  int cut = content.length();
  int lines = 0;
  for (int i = content.length() - 1; i >= 0; i--) {
    if (content.charAt(i) == '\n') {
      lines++;
      if (lines > 150) {
        cut = i + 1;
        break;
      }
    }
  }
  if (cut <= 0 || cut >= (int)content.length())
    return;
  content = content.substring(cut);
  f = LittleFS.open(SHOTS_LOG_PATH, FILE_WRITE);
  if (f) {
    f.print(content);
    f.close();
  }
}

static void appendShotHistory(int durationSec) {
  if (!LittleFS.begin())
    return;
  time_t now = time(nullptr);
  if (now < 1700000000)
    now = 0;
  int hx = hxTemperature;
  int st = steamTemperature;
  File file = LittleFS.open(SHOTS_LOG_PATH, FILE_APPEND);
  if (!file)
    return;
  file.printf("%lld,%d,%d,%d\n", (long long)now, durationSec, hx, st);
  file.close();
  trimShotsLogIfNeeded();
}

static void setupWifiAndServices() {
  if (!wifiCredentialsConfigured()) {
    startConfigPortal();
    return;
  }
  if (!tryConnectSta()) {
    Serial.println("WLAN: Verbindung fehlgeschlagen -> Konfigurations-AP");
    startConfigPortal();
    return;
  }
  syncNetworkTime();
  if (MDNS.begin("marax")) {
    MDNS.addService("http", "tcp", 80);
  }
  if (MQTT_BROKER[0] != '\0') {
    mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  }
}

static void maybeReconnectWifi() {
  if (g_configPortalMode)
    return;
  if (!wifiCredentialsConfigured())
    return;
  if (WiFi.status() == WL_CONNECTED)
    return;
  static unsigned long lastMs = 0;
  if (millis() - lastMs < 30000)
    return;
  lastMs = millis();
  WiFi.reconnect();
}

static void handleApiHistory();
static void handleClearHistory();
static void handleRootPage();
static void handleUpdateGet();
static void handleUpdatePostDone();
static void handleUpdateUpload();
static void sendWifiConfigPage();
static void handleSaveWifi();
static void handleWifiGet();

static bool otaUpdateConfigured() {
  return OTA_UPDATE_TOKEN[0] != '\0';
}

static void setupWebServer() {
  server.on("/", HTTP_GET, handleRootPage);
  server.on("/wifi", HTTP_GET, handleWifiGet);
  server.on("/save-wifi", HTTP_POST, handleSaveWifi);
  server.on("/api/history", HTTP_GET, handleApiHistory);
  server.on("/api/clear", HTTP_POST, handleClearHistory);
  server.on("/update", HTTP_GET, handleUpdateGet);
  server.on(
      "/update", HTTP_POST,
      []() {
        handleUpdatePostDone();
      },
      []() {
        handleUpdateUpload();
      });
  server.onNotFound([]() {
    server.send(404, "text/plain", "404");
  });
  server.begin();
  g_webServerStarted = true;
}

static void sendWifiConfigPage() {
  String html;
  html.reserve(2200);
  html += F("<!DOCTYPE html><html><head><meta charset=\"utf-8\">");
  html += F("<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">");
  html += F("<title>WLAN einrichten</title>");
  html += F("<style>body{font-family:system-ui,sans-serif;max-width:22rem;margin:1rem auto}");
  html += F("label{display:block;margin:.6rem 0}input{width:100%;box-sizing:border-box;padding:.35rem}");
  html += F("button{margin-top:.5rem;padding:.45rem .9rem}</style></head><body>");
  if (g_configPortalMode) {
    html += F("<h1>MaraX-Timer</h1><p>Zuerst mit diesem WLAN verbinden:</p>");
    html += F("<p><strong>SSID:</strong> MaraX-Timer</p>");
    if (strlen(CONFIG_AP_PASSWORD) >= 8) {
      html += F("<p><strong>Passwort:</strong> wie <code>CONFIG_AP_PASSWORD</code> im Sketch (Standard siehe Dokumentation).</p>");
    } else {
      html += F("<p><em>Offenes WLAN (Passwort im Sketch setzen empfohlen).</em></p>");
    }
    html += F("<p>Dann hier Heim-WLAN eintragen (Router meist <strong>192.168.4.1</strong>):</p>");
  } else {
    html += F("<h1>WLAN &auml;ndern</h1>");
    if (WiFi.status() == WL_CONNECTED) {
      html += F("<p>Verbunden mit: <strong>");
      html += WiFi.SSID();
      html += F("</strong></p>");
    }
  }
  html += F("<form method=\"post\" action=\"/save-wifi\" accept-charset=\"utf-8\">");
  html += F("<label>SSID (Heim-WLAN)<input name=\"ssid\" required maxlength=\"32\" value=\"");
  if (!g_configPortalMode && WiFi.status() == WL_CONNECTED) {
    html += WiFi.SSID();
  }
  html += F("\"></label>");
  html += F("<label>Passwort<input name=\"pass\" type=\"password\" maxlength=\"64\" "
            "autocomplete=\"off\" placeholder=\"leer = offenes WLAN\"></label>");
  html += F("<p><button type=\"submit\">Speichern &amp; Ger&auml;t neu starten</button></p></form>");
  html += F("<p><a href=\"/\">Zur&uuml;ck</a></p></body></html>");
  server.send(200, "text/html; charset=utf-8", html);
}

static void handleSaveWifi() {
  if (!server.hasArg("ssid")) {
    server.send(400, "text/plain; charset=utf-8", "ssid fehlt");
    return;
  }
  String ssid = server.arg("ssid");
  String pass = server.arg("pass");
  ssid.trim();
  if (ssid.isEmpty()) {
    server.send(400, "text/plain; charset=utf-8", "SSID leer");
    return;
  }
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  server.send(200, "text/html; charset=utf-8",
                "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
                "<meta http-equiv=\"refresh\" content=\"2;url=/\">"
                "</head><body><p>Gespeichert. Neustart …</p></body></html>");
  delay(400);
  ESP.restart();
}

static void handleWifiGet() {
  if (g_configPortalMode) {
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "OK");
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    server.send(503, "text/plain; charset=utf-8", "WLAN nicht verbunden");
    return;
  }
  sendWifiConfigPage();
}

static void handleRootPage() {
  if (g_configPortalMode) {
    sendWifiConfigPage();
    return;
  }
  String html;
  html.reserve(3000);
  html += F("<!DOCTYPE html><html><head><meta charset=\"utf-8\">");
  html += F("<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">");
  html += F("<title>MaraX Timer</title><style>");
  html += F("body{font-family:system-ui,sans-serif;max-width:40rem;margin:1rem auto}");
  html += F("table{border-collapse:collapse;width:100%}th,td{border:1px solid #ccc;padding:.4rem}");
  html += F("th{background:#eee}button{padding:.4rem .8rem;margin-top:.5rem}</style></head><body>");
  html += F("<h1>MaraX Timer</h1><p>");
  if (WiFi.status() == WL_CONNECTED) {
    html += F("IP: ");
    html += WiFi.localIP().toString();
    html += F(" &middot; <a href=\"http://marax.local\">mdns: marax.local</a>");
  } else {
    html += F("WLAN: nicht verbunden");
  }
  html += F("</p><h2>Shot-Verlauf (lokal, ohne Home Assistant)</h2>");
  html += F("<p>Nur Shots &ge; 15 s (wie Kurzsp&uuml;lung-Filter). Zeit: UTC nach NTP.</p>");
  html += F("<table><thead><tr><th>Zeit (UTC)</th><th>Sekunden</th><th>HX &deg;C</th>");
  html += F("<th>Dampf &deg;C</th></tr></thead><tbody id=\"t\"></tbody></table>");
  html += F("<p><button type=\"button\" id=\"clr\">Verlauf l&ouml;schen</button></p>");
  html += F("<script>");
  html += F("function esc(s){return s.replace(/[&<>'\"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;',\"'\":'&#39;','\"':'&quot;'}[c]));}");
  html += F("fetch('/api/history').then(r=>r.json()).then(d=>{");
  html += F("const t=document.getElementById('t');");
  html += F("const a=Array.isArray(d)?d.slice():[];a.reverse();");
  html += F("a.forEach(x=>{const tr=document.createElement('tr');");
  html += F("const iso=x.t>1700000000?new Date(x.t*1000).toISOString().replace('T',' ').slice(0,19):'(kein NTP)';");
  html += F("tr.innerHTML='<td>'+esc(iso)+'</td><td>'+x.s+'</td><td>'+(x.hx!=null?x.hx:'-')+'</td><td>'+(x.st!=null?x.st:'-')+'</td>';");
  html += F("t.appendChild(tr)});});");
  html += F("document.getElementById('clr').onclick=()=>fetch('/api/clear',{method:'POST'})");
  html += F(".then(()=>location.reload());");
  html += F("</script>");
  html += F("<p style=\"margin-top:1.5rem\">");
  html += F("<a href=\"/wifi\">WLAN &auml;ndern</a> &middot; ");
  html += F("<a href=\"/update\">Firmware aktualisieren (OTA)</a></p>");
  html += F("</body></html>");
  server.send(200, "text/html; charset=utf-8", html);
}

static void handleUpdateGet() {
  if (!g_configPortalMode && WiFi.status() != WL_CONNECTED) {
    server.send(503, "text/plain; charset=utf-8", "WLAN nicht verbunden");
    return;
  }
  if (!otaUpdateConfigured()) {
    server.send(403, "text/html; charset=utf-8",
                "<!DOCTYPE html><html><head><meta charset=utf-8><title>OTA</title></head>"
                "<body><p>OTA ist deaktiviert. Setze <code>OTA_UPDATE_TOKEN</code> im "
                "Sketch und lade neu.</p><p><a href=\"/\">Zur&uuml;ck</a></p></body></html>");
    return;
  }
  server.send(
      200, "text/html; charset=utf-8",
      "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
      "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
      "<title>Firmware-Update</title>"
      "<style>body{font-family:system-ui,sans-serif;max-width:28rem;margin:1rem auto}"
      "label{display:block;margin:.5rem 0}button{margin-top:1rem;padding:.5rem 1rem}</style>"
      "</head><body><h1>Firmware-Update</h1>"
      "<p>Binary aus der Arduino-IDE: <em>Sketch &rarr; Export Compiled Binary</em> "
      "(<code>.bin</code> im Sketch-Ordner).</p>"
      "<form id=\"ota\" method=\"post\" enctype=\"multipart/form-data\">"
      "<label>Token <input type=\"password\" id=\"tok\" required autocomplete=\"off\"></label>"
      "<label>Firmware <input type=\"file\" name=\"firmware\" accept=\".bin\" required></label>"
      "<button type=\"submit\">Hochladen &amp; Neustart</button></form>"
      "<p><a href=\"/\">Zur&uuml;ck</a></p>"
      "<script>document.getElementById('ota').onsubmit=function(e){"
      "e.preventDefault();var t=document.getElementById('tok').value;"
      "if(!t)return;this.action='/update?token='+encodeURIComponent(t);this.submit();"
      "};</script></body></html>");
}

static void handleUpdateUpload() {
  HTTPUpload &upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    otaUploadSucceeded = false;
    if (!otaUpdateConfigured()) {
      return;
    }
    if (!server.hasArg("token") ||
        String(OTA_UPDATE_TOKEN) != server.arg("token")) {
      Serial.println("OTA: ungültiges Token");
      return;
    }
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      Update.printError(Serial);
      return;
    }
    return;
  }
  if (upload.status == UPLOAD_FILE_WRITE) {
    if (Update.isRunning()) {
      size_t w = Update.write(upload.buf, upload.currentSize);
      if (w != upload.currentSize) {
        Update.printError(Serial);
      }
    }
    return;
  }
  if (upload.status == UPLOAD_FILE_END) {
    if (Update.isRunning()) {
      if (Update.end(true)) {
        otaUploadSucceeded = true;
      } else {
        Update.printError(Serial);
      }
    }
    return;
  }
  if (upload.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
  }
}

static void handleUpdatePostDone() {
  if (!otaUpdateConfigured()) {
    server.send(403, "text/plain; charset=utf-8", "OTA deaktiviert");
    return;
  }
  if (!server.hasArg("token") ||
      String(OTA_UPDATE_TOKEN) != server.arg("token")) {
    server.send(403, "text/plain; charset=utf-8", "Token ungültig");
    return;
  }
  if (!otaUploadSucceeded) {
    server.send(400, "text/plain; charset=utf-8",
                "Kein gültiges Firmware-Image (Token prüfen, .bin vollständig?).");
    return;
  }
  server.sendHeader("Connection", "close");
  server.send(200, "text/plain; charset=utf-8",
                "OK. Gerät startet neu … Seite neu laden, wenn die IP gleich bleibt.");
  delay(200);
  ESP.restart();
}

static void handleApiHistory() {
  if (!LittleFS.begin()) {
    server.send(200, "application/json", "[]");
    return;
  }
  File f = LittleFS.open(SHOTS_LOG_PATH, "r");
  if (!f) {
    server.send(200, "application/json", "[]");
    return;
  }
  String json = "[";
  bool first = true;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.isEmpty())
      continue;
    long long ts = 0;
    int dur = 0, hx = MARAX_TEMP_INVALID, st = MARAX_TEMP_INVALID;
    int n = sscanf(line.c_str(), "%lld,%d,%d,%d", &ts, &dur, &hx, &st);
    if (n < 2)
      continue;
    if (!first)
      json += ",";
    first = false;
    json += "{\"t\":";
    json += String((unsigned long)ts);
    json += ",\"s\":";
    json += String(dur);
    if (n >= 3 && hx > MARAX_TEMP_INVALID + 1 && hx < 200) {
      json += ",\"hx\":";
      json += String(hx);
    }
    if (n >= 4 && st > MARAX_TEMP_INVALID + 1 && st < 300) {
      json += ",\"st\":";
      json += String(st);
    }
    json += "}";
  }
  f.close();
  json += "]";
  server.send(200, "application/json", json);
}

static void handleClearHistory() {
  LittleFS.remove(SHOTS_LOG_PATH);
  server.sendHeader("Location", "/");
  server.send(303);
}

static unsigned long lastMqttConnectAttemptMs = 0;

static void ensureMqttConnected() {
  if (!mqttConfigured())
    return;
  if (WiFi.status() != WL_CONNECTED)
    return;
  if (mqttClient.connected())
    return;
  if (millis() - lastMqttConnectAttemptMs < 5000)
    return;
  lastMqttConnectAttemptMs = millis();
  if (MQTT_USER[0] != '\0') {
    mqttClient.connect("marax-esp32", MQTT_USER, MQTT_PASSWORD);
  } else {
    mqttClient.connect("marax-esp32");
  }
}

static void broadcastMachineState() {
  if (machineState == "off") {
    mqttClient.publish("/marax/power", "off");
  } else {
    mqttClient.publish("/marax/power", "on");
  }
}

static void broadcastHxTemperature() {
  mqttClient.publish("/marax/hx", String(hxTemperature).c_str());
}

static void broadcastSteamTemperature() {
  mqttClient.publish("/marax/steam", String(steamTemperature).c_str());
}

static void broadcastShot() {
  mqttClient.publish("/marax/shot", String(timerCount).c_str());
}

static void broadcastPump() {
  mqttClient.publish("/marax/pump", timerStarted ? "on" : "off");
}

static void broadcastMachineHeating() {
  mqttClient.publish("/marax/machineheating", machineHeating ? "on" : "off");
}

static void broadcastMachineHeatingBoost() {
  mqttClient.publish("/marax/machineheatingboost", machineHeatingBoost ? "on" : "off");
}

static void syncTimerCountForMqtt() {
  if (timerStarted) {
    timerCount = (millis() - timerStartMillis) / 1000;
    if (timerCount > 15) {
      prevTimerCount = timerCount;
    }
  } else {
    timerCount = prevTimerCount;
  }
}

void updateMqtt() {
  if (!mqttConfigured())
    return;

  syncTimerCountForMqtt();

  ensureMqttConnected();
  if (!mqttClient.connected())
    return;
  mqttClient.loop();

  if (lastMachineState != machineState) {
    lastMachineState = machineState;
    broadcastMachineState();
  }

  if (lastHxTemperature != hxTemperature) {
    lastHxTemperature = hxTemperature;
    if (hxTemperature > 15 && hxTemperature < 115 &&
        abs(hxTemperature - lastHxTemperature) < 3) {
      broadcastHxTemperature();
    }
  }

  if (lastSteamTemperature != steamTemperature) {
    lastSteamTemperature = steamTemperature;
    if (steamTemperature > 15 && steamTemperature < 250 &&
        abs(steamTemperature - lastSteamTemperature) < 3) {
      broadcastSteamTemperature();
    }
  }

  if (lastTimerStartMillis != timerStartMillis &&
      ((millis() - timerStartMillis) / 1000) > 15 && !timerStarted &&
      timerCount > 0) {
    lastTimerStartMillis = timerStartMillis;
    broadcastShot();
  }

  if (lastTimerStarted != timerStarted) {
    lastTimerStarted = timerStarted;
    broadcastPump();
  }

  if (lastMachineHeating != machineHeating) {
    lastMachineHeating = machineHeating;
    broadcastMachineHeating();
  }

  if (lastMachineHeatingBoost != machineHeatingBoost) {
    lastMachineHeatingBoost = machineHeatingBoost;
    broadcastMachineHeatingBoost();
  }
}

static String getMachineStateFromLine() {
  if (String(receivedChars[0]) == "C") {
    return "C";
  }
  if (String(receivedChars[0]) == "V") {
    return "S";
  }
  return "X";
}

static bool getMachineHeatingFromLine() {
  return String(receivedChars[23]) == "1";
}

static bool getMachineHeatingBoostFromLine() {
  return String(receivedChars).substring(18, 22) == "0000";
}

static int getTemperatureHxFromLine() {
  if (receivedChars[14] && receivedChars[15] && receivedChars[16]) {
    return String(receivedChars).substring(14, 17).toInt();
  }
  return MARAX_TEMP_INVALID;
}

static int getTemperatureSteamFromLine() {
  if (receivedChars[6] && receivedChars[7] && receivedChars[8]) {
    return String(receivedChars).substring(6, 9).toInt();
  }
  return MARAX_TEMP_INVALID;
}

static void applyMachineTelemetryFromLine() {
  machineState = getMachineStateFromLine();
  machineHeating = getMachineHeatingFromLine();
  machineHeatingBoost = getMachineHeatingBoostFromLine();
  hxTemperature = getTemperatureHxFromLine();
  steamTemperature = getTemperatureSteamFromLine();
}

void setup() {
  LittleFS.begin(true);
  loadWifiCredentials();
  setupWifiAndServices();
  if (g_configPortalMode || WiFi.status() == WL_CONNECTED) {
    setupWebServer();
  }

  Wire.begin(I2C_SDA, I2C_SCL);

  Serial.begin(9600);
  MaraXSerial.begin(9600, SERIAL_8N1, MARAX_RX, MARAX_TX);

  pinMode(REED_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(REED_PIN), onReedChanged, CHANGE);

  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);

  t.every(100, updateDisplay);

  memset(receivedChars, 0, numChars);

  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  display.clearDisplay();
  display.setTextColor(WHITE);
  display.display();
  MaraXSerial.write(0x11);
}

void loop() {
  t.update();
  detectChanges();
  getMachineInput();
  maybeReconnectWifi();
  if (g_webServerStarted) {
    server.handleClient();
  }
  updateMqtt();
}

void getMachineInput() {
  while (MaraXSerial.available()) {
    serialUpdateMillis = millis();
    rc = MaraXSerial.read();

    if (rc != endMarker) {
      receivedChars[ndx] = rc;
      ndx++;
      if (ndx >= numChars) {
        ndx = numChars - 1;
      }
    } else {
      receivedChars[ndx] = '\0';
      ndx = 0;
      applyMachineTelemetryFromLine();
      Serial.println(receivedChars);
    }
  }

  if (millis() - serialUpdateMillis > 5000) {
    serialUpdateMillis = millis();
    memset(receivedChars, 0, numChars);
    Serial.println("Request serial update");
    MaraXSerial.write(0x11);
    machineState = "off";
    machineHeating = false;
    machineHeatingBoost = false;
    hxTemperature = MARAX_TEMP_INVALID;
    steamTemperature = MARAX_TEMP_INVALID;
  }
}

void detectChanges() {
  if (reedInterruptPending) {
    reedInterruptPending = false;
  }

  digitalWrite(LED_BUILTIN, digitalRead(REED_PIN));

  if (reedOpenSensor) {
    pumpInValue = digitalRead(REED_PIN);
  } else {
    pumpInValue = !digitalRead(REED_PIN);
  }

  if (!timerStarted && !pumpInValue) {
    timerStartMillis = millis();
    timerStarted = true;
    displayOn = true;
    Serial.println("Start pump");
  }
  if (timerStarted && pumpInValue) {
    if (timerStopMillis == 0) {
      timerStopMillis = millis();
    }
    if (millis() - timerStopMillis > 500) {
      int durationSec = (int)((millis() - timerStartMillis) / 1000);
      if (durationSec >= 15) {
        appendShotHistory(durationSec);
      }
      timerStarted = false;
      timerStopMillis = 0;
      timerDisplayOffMillis = millis();
      display.invertDisplay(false);
      Serial.println("Stop pump");
    }
  } else {
    timerStopMillis = 0;
  }
  if (!timerStarted && displayOn && timerDisplayOffMillis >= 0 &&
      (millis() - timerDisplayOffMillis > 1000L * 60L * 60L)) {
    timerDisplayOffMillis = 0;
    timerCount = 0;
    prevTimerCount = 0;
    displayOn = false;
    Serial.println("Sleep");
  }
}

String getTimer() {
  char outMin[3];
  if (timerStarted) {
    timerCount = (millis() - timerStartMillis) / 1000;
    if (timerCount > 15) {
      prevTimerCount = timerCount;
    }
  } else {
    timerCount = prevTimerCount;
  }
  if (timerCount > 99) {
    return "99";
  }
  sprintf(outMin, "%02u", timerCount);
  return outMin;
}

void updateDisplay() {
  display.clearDisplay();
  if (displayOn) {
    if (timerStarted) {
      display.setTextSize(7);
      display.setCursor(25, 8);
      display.print(getTimer());
    } else {
      display.drawLine(74, 0, 74, 63, SSD1306_WHITE);
      display.setTextSize(4);
      display.setCursor(display.width() / 2 - 1 + 17, 20);
      display.print(getTimer());
      if (receivedChars[0]) {
        display.setTextSize(2);
        display.setCursor(1, 1);
        if (String(receivedChars[0]) == "C") {
          display.print("C");
        } else if (String(receivedChars[0]) == "V") {
          display.print("S");
        } else {
          display.print("X");
        }
      }
      if (String(receivedChars).substring(18, 22) == "0000") {
        if (String(receivedChars[23]) == "1") {
          display.fillCircle(45, 7, 6, SSD1306_WHITE);
        }
        if (String(receivedChars[23]) == "0") {
          display.drawCircle(45, 7, 6, SSD1306_WHITE);
        }
      } else {
        if (String(receivedChars[23]) == "1") {
          display.fillRect(39, 1, 12, 12, SSD1306_WHITE);
        }
        if (String(receivedChars[23]) == "0") {
          display.drawRect(39, 1, 12, 12, SSD1306_WHITE);
        }
      }
      if (receivedChars[14] && receivedChars[15] && receivedChars[16]) {
        display.setTextSize(3);
        display.setCursor(1, 20);
        if (String(receivedChars[14]) != "0") {
          display.print(String(receivedChars[14]));
        }
        display.print(String(receivedChars[15]));
        display.print(String(receivedChars[16]));
        display.print((char)247);
        if (String(receivedChars[14]) == "0") {
          display.print("C");
        }
      }
      if (receivedChars[6] && receivedChars[7] && receivedChars[8]) {
        display.setTextSize(2);
        display.setCursor(1, 48);
        if (String(receivedChars[6]) != "0") {
          display.print(String(receivedChars[6]));
        }
        display.print(String(receivedChars[7]));
        display.print(String(receivedChars[8]));
        display.print((char)247);
        display.print("C");
      }
    }
  }
  display.display();
}
