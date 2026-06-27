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
// SSD1306 I2C: die meisten 128x64-Module 0x3C, manche 0x3D — bei schwarzem Display wechseln.
#define SSD1306_I2C_ADDR 0x3C
#define MARAX_RX 14
#define MARAX_TX 12

// DevKit V1: Onboard-LED meist GPIO 2. FQBN esp32:esp32:esp32 setzt oft kein LED_BUILTIN.
#ifndef LED_BUILTIN
#define LED_BUILTIN 2
#endif

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
const char *OTA_UPDATE_TOKEN = "maraxota";

#define FIRMWARE_VERSION "1.1.0"
static const char FIRMWARE_BUILT[] = __DATE__ " " __TIME__;

#define MARAX_TEMP_INVALID (-1000)

Adafruit_SSD1306 display(128, 64, &Wire, -1);
HardwareSerial MaraXSerial(2);

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

int targetShotSeconds = 27;
int targetHxTemp = 93;

static bool g_demoMode = false;
static unsigned long g_demoStartMs = 0;

static bool g_showSparkline = false;
static unsigned long g_lastDisplaySwitch = 0;
static bool g_wasAtTemp = false;
static unsigned long g_readySinceMs = 0;

#define TEMP_HISTORY_SIZE 120
struct TempSample { uint32_t ms; int16_t hx; int16_t steam; };
static TempSample tempHistory[TEMP_HISTORY_SIZE];
static int tempHistoryHead = 0;
static int tempHistoryCount = 0;
static unsigned long lastTempSampleMs = 0;

// Pump-Source: false = Reed-GPIO (default), true = UART-Serielldaten
bool g_useSerialPump = false;
bool g_serialPumpActive = false;

// Fallback-AP: läuft parallel zum Heimnetz; per API/UI abschaltbar
bool g_apEnabled = true;

// true/false je nach Reed-Typ (Modul / Schaltlogik); bei falscher Polarität anpassen
bool reedOpenSensor = true;
bool displayOn = true;
int timerCount = 0;
int prevTimerCount = 0;
bool shotRunning = false;
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
  targetShotSeconds = prefs.getInt("targshot", 27);
  targetHxTemp = prefs.getInt("targhx", 93);
  g_useSerialPump = prefs.getBool("serialpump", false);
  g_apEnabled = prefs.getBool("apon", true);
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
  if (g_apEnabled) {
    WiFi.mode(WIFI_AP_STA);
    if (strlen(CONFIG_AP_PASSWORD) >= 8) {
      WiFi.softAP("MaraX-Timer", CONFIG_AP_PASSWORD);
    } else {
      WiFi.softAP("MaraX-Timer");
    }
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

static void handleApiDemo();
static void handleApiHistory();
static void handleApiHistoryCsv();
static void handleApiStatus();
static void handleApiWifiScan();
static void handleApiTempHistory();
static void handleApiSaveTarget();
static void handleTestPage();
static void handleTempsPage();
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
  server.on("/api/demo", HTTP_POST, handleApiDemo);
  server.on("/api/status", HTTP_GET, handleApiStatus);
  server.on("/api/wifi-scan", HTTP_GET, handleApiWifiScan);
  server.on("/api/temp-history", HTTP_GET, handleApiTempHistory);
  server.on("/api/history.csv", HTTP_GET, handleApiHistoryCsv);
  server.on("/api/set-target", HTTP_POST, handleApiSaveTarget);
  server.on("/test", HTTP_GET, handleTestPage);
  server.on("/temps", HTTP_GET, handleTempsPage);
  server.on("/api/clear", HTTP_POST, handleClearHistory);
  server.on("/api/set-pump-source", HTTP_POST, handleApiSetPumpSource);
  server.on("/api/set-ap", HTTP_POST, handleApiSetAp);
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
  html += F("<label>SSID (Heim-WLAN)<input id=\"ssid\" name=\"ssid\" required maxlength=\"32\" value=\"");
  if (!g_configPortalMode && WiFi.status() == WL_CONNECTED) {
    html += WiFi.SSID();
  }
  html += F("\"></label>");
  html += F("<div id=\"nets\" style=\"margin:.4rem 0;font-size:.9rem\"><em>Netzwerke werden gesucht...</em></div>");
  html += F("<label>Passwort<input name=\"pass\" type=\"password\" maxlength=\"64\" "
            "autocomplete=\"off\" placeholder=\"leer = offenes WLAN\"></label>");
  html += F("<p><button type=\"submit\">Speichern &amp; Ger&auml;t neu starten</button></p></form>");
  html += F("<script>");
  html += F("var bars=['','&#9601;','&#9601;&#9603;','&#9601;&#9603;&#9605;','&#9601;&#9603;&#9605;&#9607;'];");
  html += F("fetch('/api/wifi-scan').then(r=>r.json()).then(nets=>{");
  html += F("var d=document.getElementById('nets');d.innerHTML='';");
  html += F("nets.sort((a,b)=>b.rssi-a.rssi);");
  html += F("nets.forEach(n=>{var s=document.createElement('span');");
  html += F("s.style='display:inline-block;margin:.2rem .3rem;padding:.2rem .5rem;background:#e8e8e8;border-radius:4px;cursor:pointer;white-space:nowrap';");
  html += F("s.innerHTML=bars[n.bars]+' '+n.ssid+(n.open?' (offen)':'');");
  html += F("s.onclick=function(){document.getElementById('ssid').value=n.ssid;};");
  html += F("d.appendChild(s);});");
  html += F("}).catch(()=>{document.getElementById('nets').innerHTML='<em>Scan fehlgeschlagen</em>';});");
  html += F("</script>");
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
  html += F("<h1>MaraX Timer</h1>");
  html += F("<div style=\"background:#f5f5f5;border-radius:8px;padding:.8rem 1rem;margin-bottom:1rem\">");
  html += F("<div style=\"display:flex;justify-content:space-between;align-items:center\">");
  html += F("<span id=\"pump\" style=\"font-size:1.1rem\">&#8226; lade...</span>");
  html += F("<span id=\"timerval\" style=\"font-size:1.8rem;font-weight:bold\"></span></div>");
  html += F("<div style=\"margin-top:.5rem;background:#ddd;border-radius:4px;height:14px;overflow:hidden\">");
  html += F("<div id=\"bar\" style=\"height:100%;width:0%;background:#aaa;transition:width .4s,background .4s\"></div></div>");
  html += F("<div style=\"display:flex;justify-content:space-between;font-size:.8rem;color:#777;margin-top:.2rem\">");
  html += F("<span>0s</span><span id=\"target-label\"></span></div></div>");
  html += F("<div style=\"margin-bottom:1rem;font-size:.9rem\">");
  html += F("Ziel-Shot: <input id=\"tshot\" type=\"number\" min=\"5\" max=\"60\" style=\"width:4rem\" value=\"");
  html += String(targetShotSeconds);
  html += F("\"> s &nbsp; HX-Ziel: <input id=\"thx\" type=\"number\" min=\"50\" max=\"130\" style=\"width:4rem\" value=\"");
  html += String(targetHxTemp);
  html += F("\"> °C &nbsp; <button onclick=\"saveTarget()\">Speichern</button></div>");
  html += F("<script>");
  html += F("var prevHx=-1;");
  html += F("if('Notification' in window && Notification.permission==='default') Notification.requestPermission();");
  html += F("function saveTarget(){");
  html += F("fetch('/api/set-target',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},");
  html += F("body:'shot='+document.getElementById('tshot').value+'&hx='+document.getElementById('thx').value})");
  html += F(".then(()=>location.reload());}");
  html += F("function upd(){fetch('/api/status').then(r=>r.json()).then(d=>{");
  html += F("var tgt=parseInt(document.getElementById('tshot').value)||27;");
  html += F("var p=document.getElementById('pump');");
  html += F("var t=document.getElementById('timerval');");
  html += F("var bar=document.getElementById('bar');");
  html += F("document.getElementById('target-label').textContent=tgt+'s Ziel';");
  html += F("if(d.pump){p.innerHTML='&#9670; Pumpe AN';p.style.color='#c00';");
  html += F("t.textContent=d.timer+'s';");
  html += F("var pct=Math.min(d.timer/tgt*100,100);");
  html += F("var col=d.timer<tgt*0.9?'#3b82f6':d.timer<=tgt*1.1?'#16a34a':'#dc2626';");
  html += F("bar.style.width=pct+'%';bar.style.background=col;}");
  html += F("else{p.innerHTML='&#9671; Pumpe aus';p.style.color='#555';");
  html += F("if(d.timer>0){t.textContent=d.timer+'s';");
  html += F("var col2=d.timer>=tgt*0.9&&d.timer<=tgt*1.1?'#16a34a':'#dc2626';");
  html += F("bar.style.width='100%';bar.style.background=col2;}");
  html += F("else{t.textContent='';bar.style.width='0%';}}");
  html += F("var thx=parseInt(document.getElementById('thx').value)||93;");
  html += F("if(prevHx<thx&&d.hx>=thx&&'Notification' in window&&Notification.permission==='granted'){");
  html += F("new Notification('Mara X bereit',{body:'HX: '+d.hx+'°C – Dampf: '+d.steam+'°C'});}");
  html += F("prevHx=d.hx;");
  html += F("});}");
  html += F("upd();setInterval(upd,500);");
  html += F("</script>");
  html += F("<p>");
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
  html += F("<a href=\"/temps\">Temperaturgraph</a> &middot; ");
  html += F("<a href=\"/test\">Diagnose</a> &middot; ");
  html += F("<a href=\"/api/history.csv\">CSV-Export</a> &middot; ");
  html += F("<a href=\"/wifi\">WLAN</a> &middot; ");
  html += F("<a href=\"/update\">OTA</a></p>");
  html += F("<p style=\"font-size:.72rem;color:#aaa;text-align:center;margin-top:1rem\">v");
  html += F(FIRMWARE_VERSION);
  html += F(" &middot; ");
  html += FIRMWARE_BUILT;
  html += F("</p>");
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

static void handleApiSetAp() {
  if (!server.hasArg("enabled")) {
    server.send(400, "application/json", "{\"error\":\"missing enabled param\"}");
    return;
  }
  g_apEnabled = (server.arg("enabled") != "0");
  prefs.begin("marax", false);
  prefs.putBool("apon", g_apEnabled);
  prefs.end();
  if (g_apEnabled && !g_configPortalMode) {
    WiFi.mode(WIFI_AP_STA);
    if (strlen(CONFIG_AP_PASSWORD) >= 8) {
      WiFi.softAP("MaraX-Timer", CONFIG_AP_PASSWORD);
    } else {
      WiFi.softAP("MaraX-Timer");
    }
  } else if (!g_apEnabled && !g_configPortalMode) {
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
  }
  server.send(200, "application/json",
    "{\"ok\":true,\"ap\":" + String(g_apEnabled ? "true" : "false") + "}");
}

static void handleApiSetPumpSource() {
  if (server.hasArg("source")) {
    g_useSerialPump = (server.arg("source") == "serial");
    prefs.begin("marax", false);
    prefs.putBool("serialpump", g_useSerialPump);
    prefs.end();
    server.send(200, "application/json",
      "{\"ok\":true,\"pumpSource\":\"" + String(g_useSerialPump ? "serial" : "reed") + "\"}");
  } else {
    server.send(400, "application/json", "{\"error\":\"missing source param\"}");
  }
}

static void handleApiStatus() {
  String json = "{";
  json += "\"pump\":" + String(shotRunning ? "true" : "false");
  json += ",\"timer\":" + String(shotRunning ? (int)((millis() - timerStartMillis) / 1000) : prevTimerCount);
  json += ",\"reed\":" + String(digitalRead(REED_PIN));
  json += ",\"hx\":" + String(hxTemperature);
  json += ",\"steam\":" + String(steamTemperature);
  json += ",\"state\":\"" + machineState + "\"";
  json += ",\"heating\":" + String(machineHeating ? "true" : "false");
  json += ",\"boost\":" + String(machineHeatingBoost ? "true" : "false");
  json += ",\"raw\":\"" + String(receivedChars) + "\"";
  json += ",\"demo\":" + String(g_demoMode ? "true" : "false");
  json += ",\"pumpSource\":\"" + String(g_useSerialPump ? "serial" : "reed") + "\"";
  json += ",\"serialPump\":" + String(g_serialPumpActive ? "true" : "false");
  json += ",\"ap\":" + String(g_apEnabled ? "true" : "false");
  json += ",\"version\":\"" FIRMWARE_VERSION "\"";
  json += ",\"built\":\"" + String(FIRMWARE_BUILT) + "\"";
  json += "}";
  server.send(200, "application/json", json);
}

static void prefillDemoTempHistory() {
  tempHistoryCount = 0;
  tempHistoryHead = 0;
  for (int i = 0; i < 24; i++) {
    int hx = 85 + i / 3;
    int st = 116 + i / 2;
    tempHistory[i] = { (uint32_t)(millis() - (23 - i) * 5000UL), (int16_t)hx, (int16_t)st };
  }
  tempHistoryCount = 24;
  tempHistoryHead = 24 % TEMP_HISTORY_SIZE;
}

static void updateDemo() {
  if (!g_demoMode) return;
  unsigned long el = (millis() - g_demoStartMs) / 1000;
  if (el >= 54) { g_demoStartMs = millis(); el = 0; }

  hxTemperature  = 88 + (int)(el < 40 ? el * 0.12 : 5);
  steamTemperature = min(118 + (int)(el * 0.35), 132);
  machineHeating  = hxTemperature < targetHxTemp;
  machineHeatingBoost = false;
  machineState   = "C";

  bool pump = el >= 4 && el < 34;
  if (pump && !shotRunning) {
    shotRunning = true;
    timerStartMillis = millis() - (long)(el - 4) * 1000L;
    timerStopMillis  = 0;
    displayOn = true;
  } else if (!pump && shotRunning) {
    shotRunning  = false;
    prevTimerCount = 30;
    timerStopMillis = 0;
    timerDisplayOffMillis = millis();
  }

  if (millis() - lastTempSampleMs >= 5000) {
    lastTempSampleMs = millis();
    tempHistory[tempHistoryHead] = { (uint32_t)millis(), (int16_t)hxTemperature, (int16_t)steamTemperature };
    tempHistoryHead = (tempHistoryHead + 1) % TEMP_HISTORY_SIZE;
    if (tempHistoryCount < TEMP_HISTORY_SIZE) tempHistoryCount++;
  }
}

static void handleApiDemo() {
  g_demoMode = !g_demoMode;
  if (g_demoMode) {
    g_demoStartMs = millis();
    shotRunning   = false;
    prefillDemoTempHistory();
  } else {
    shotRunning    = false;
    hxTemperature  = MARAX_TEMP_INVALID;
    steamTemperature = MARAX_TEMP_INVALID;
    machineState   = "";
    machineHeating = false;
  }
  server.send(200, "application/json", g_demoMode ? "{\"demo\":true}" : "{\"demo\":false}");
}

static void handleApiHistoryCsv() {
  if (!LittleFS.begin()) { server.send(200, "text/csv", ""); return; }
  File f = LittleFS.open(SHOTS_LOG_PATH, "r");
  if (!f) { server.send(200, "text/csv", "timestamp_utc,duration_s,hx_c,steam_c\r\n"); return; }
  String csv = "timestamp_utc,duration_s,hx_c,steam_c\r\n";
  while (f.available()) {
    String line = f.readStringUntil('\n'); line.trim();
    if (line.isEmpty()) continue;
    long long ts = 0; int dur = 0, hx = MARAX_TEMP_INVALID, st = MARAX_TEMP_INVALID;
    sscanf(line.c_str(), "%lld,%d,%d,%d", &ts, &dur, &hx, &st);
    csv += String((unsigned long)ts) + "," + String(dur) + ",";
    csv += (hx > MARAX_TEMP_INVALID + 1 ? String(hx) : "") + ",";
    csv += (st > MARAX_TEMP_INVALID + 1 ? String(st) : "") + "\r\n";
  }
  f.close();
  server.sendHeader("Content-Disposition", "attachment; filename=\"shots.csv\"");
  server.send(200, "text/csv; charset=utf-8", csv);
}

static void handleApiTempHistory() {
  String json = "{\"targetHx\":" + String(targetHxTemp) + ",\"samples\":[";
  int start = tempHistoryCount < TEMP_HISTORY_SIZE ? 0 : tempHistoryHead;
  for (int i = 0; i < tempHistoryCount; i++) {
    int idx = (start + i) % TEMP_HISTORY_SIZE;
    if (i > 0) json += ",";
    json += "{\"ms\":" + String(tempHistory[idx].ms);
    json += ",\"hx\":" + String(tempHistory[idx].hx);
    json += ",\"st\":" + String(tempHistory[idx].steam) + "}";
  }
  json += "]}";
  server.send(200, "application/json", json);
}

static void handleApiSaveTarget() {
  if (server.hasArg("shot")) {
    int v = server.arg("shot").toInt();
    if (v >= 5 && v <= 60) { targetShotSeconds = v; prefs.putInt("targshot", v); }
  }
  if (server.hasArg("hx")) {
    int v = server.arg("hx").toInt();
    if (v >= 50 && v <= 130) { targetHxTemp = v; prefs.putInt("targhx", v); }
  }
  server.send(200, "application/json", "{\"ok\":true}");
}

static void handleTempsPage() {
  String html;
  html.reserve(3000);
  html += F("<!DOCTYPE html><html><head><meta charset=\"utf-8\">");
  html += F("<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">");
  html += F("<title>Temperaturen</title><style>");
  html += F("body{font-family:system-ui,sans-serif;max-width:40rem;margin:1rem auto;padding:0 .5rem}");
  html += F("canvas{width:100%;max-width:100%;border:1px solid #ddd;border-radius:6px;display:block}");
  html += F(".leg{display:flex;gap:1rem;margin:.4rem 0;font-size:.9rem}");
  html += F(".dot{width:12px;height:12px;border-radius:50%;display:inline-block;margin-right:4px}</style></head><body>");
  html += F("<h1>Temperaturen</h1>");
  html += F("<canvas id=\"c\" height=\"220\"></canvas>");
  html += F("<div class=\"leg\"><span><span class=\"dot\" style=\"background:#2563eb\"></span>Brühkreis (HX)</span>");
  html += F("<span><span class=\"dot\" style=\"background:#dc2626\"></span>Dampf</span>");
  html += F("<span><span class=\"dot\" style=\"background:#16a34a;border-radius:0\">&#8212;</span> Ziel HX</span></div>");
  html += F("<p id=\"info\" style=\"font-size:.85rem;color:#666\">Letzte 10 Minuten (5s Aufl&ouml;sung)</p>");
  html += F("<p><a href=\"/\">&#8592; Zur&uuml;ck</a></p>");
  html += F("<script>");
  html += F("const c=document.getElementById('c');");
  html += F("function draw(d){");
  html += F("const W=c.offsetWidth||600,H=220,PAD={t:10,r:10,b:30,l:42};");
  html += F("c.width=W;c.height=H;");
  html += F("const ctx=c.getContext('2d');");
  html += F("ctx.clearRect(0,0,W,H);");
  html += F("const s=d.samples;if(!s||!s.length)return;");
  html += F("const t0=s[0].ms,t1=s[s.length-1].ms;");
  html += F("const tspan=Math.max(t1-t0,1);");
  html += F("const allT=[...s.map(x=>x.hx),...s.map(x=>x.st),d.targetHx];");
  html += F("const minT=Math.min(...allT)-5,maxT=Math.max(...allT)+5;");
  html += F("const tspan2=Math.max(maxT-minT,1);");
  html += F("const px=(t)=>PAD.l+(t-t0)/tspan*(W-PAD.l-PAD.r);");
  html += F("const py=(v)=>PAD.t+(maxT-v)/tspan2*(H-PAD.t-PAD.b);");
  html += F("ctx.strokeStyle='#e5e7eb';ctx.lineWidth=1;");
  html += F("for(let v=Math.ceil(minT/10)*10;v<=maxT;v+=10){");
  html += F("ctx.beginPath();ctx.moveTo(PAD.l,py(v));ctx.lineTo(W-PAD.r,py(v));ctx.stroke();");
  html += F("ctx.fillStyle='#6b7280';ctx.font='11px system-ui';ctx.textAlign='right';");
  html += F("ctx.fillText(v+'°',PAD.l-4,py(v)+4);}");
  html += F("ctx.setLineDash([4,3]);ctx.strokeStyle='#16a34a';ctx.lineWidth=1.5;");
  html += F("ctx.beginPath();ctx.moveTo(PAD.l,py(d.targetHx));ctx.lineTo(W-PAD.r,py(d.targetHx));ctx.stroke();");
  html += F("ctx.setLineDash([]);");
  html += F("function line(key,color){ctx.beginPath();ctx.strokeStyle=color;ctx.lineWidth=2;");
  html += F("s.forEach((p,i)=>{i===0?ctx.moveTo(px(p.ms),py(p[key])):ctx.lineTo(px(p.ms),py(p[key]));});");
  html += F("ctx.stroke();}");
  html += F("line('hx','#2563eb');line('st','#dc2626');}");
  html += F("function upd(){fetch('/api/temp-history').then(r=>r.json()).then(d=>{");
  html += F("draw(d);");
  html += F("const last=d.samples[d.samples.length-1];");
  html += F("if(last)document.getElementById('info').textContent=");
  html += F("'HX: '+last.hx+'°C  Dampf: '+last.st+'°C  ('+d.samples.length+' Messpunkte)'");
  html += F("});}");
  html += F("upd();setInterval(upd,5000);");
  html += F("</script></body></html>");
  server.send(200, "text/html; charset=utf-8", html);
}

static void handleTestPage() {
  String html;
  html.reserve(3500);
  html += F("<!DOCTYPE html><html><head><meta charset=\"utf-8\">");
  html += F("<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">");
  html += F("<title>MaraX Test</title><style>");
  html += F("body{font-family:system-ui,sans-serif;max-width:32rem;margin:1rem auto;padding:0 .5rem}");
  html += F(".card{background:#f5f5f5;border-radius:8px;padding:.8rem 1rem;margin:.5rem 0}");
  html += F(".label{font-size:.8rem;color:#777;text-transform:uppercase;letter-spacing:.05em}");
  html += F(".val{font-size:2rem;font-weight:bold;margin:.1rem 0}");
  html += F(".row{display:flex;gap:.5rem;flex-wrap:wrap}");
  html += F(".row .card{flex:1;min-width:7rem}");
  html += F(".on{color:#c00}.off{color:#555}.warn{color:#e60}</style></head><body>");
  html += F("<h1>Diagnose / Test</h1>");
  html += F("<p><button id=\"demobtn\" onclick=\"toggleDemo()\" style=\"padding:.4rem .9rem\">Demo-Shot starten</button>");
  html += F(" <span id=\"demostatus\" style=\"font-size:.85rem;color:#777\"></span></p>");
  html += F("<p><button id=\"apbtn\" onclick=\"toggleAp()\" style=\"padding:.4rem .9rem\">AP aus</button>");
  html += F(" <span style=\"font-size:.85rem;color:#777\">Fallback-AP (192.168.4.1) &mdash; bei Heimnetz-Ausfall immer aktiv</span></p>");
  html += F("<div class=\"row\">");
  html += F("<div class=\"card\"><div class=\"label\">Reed-Pin (GPIO 18)</div><div class=\"val\" id=\"reed\">-</div></div>");
  html += F("<div class=\"card\"><div class=\"label\">Pumpe</div><div class=\"val\" id=\"pump\">-</div></div>");
  html += F("<div class=\"card\"><div class=\"label\">Timer</div><div class=\"val\" id=\"timer\">-</div></div>");
  html += F("</div><div class=\"row\">");
  html += F("<div class=\"card\"><div class=\"label\">Brühkreis (HX)</div><div class=\"val\" id=\"hx\">-</div></div>");
  html += F("<div class=\"card\"><div class=\"label\">Dampf</div><div class=\"val\" id=\"steam\">-</div></div>");
  html += F("<div class=\"card\"><div class=\"label\">Maschine</div><div class=\"val\" id=\"state\">-</div></div>");
  html += F("</div>");
  html += F("<div class=\"card\"><div class=\"label\">Heizung</div><div id=\"heat\">-</div></div>");
  html += F("<div class=\"row\">");
  html += F("<div class=\"card\"><div class=\"label\">Pump-Quelle</div><div class=\"val\" id=\"psrc\">-</div></div>");
  html += F("<div class=\"card\"><div class=\"label\">Seriell Pumpe</div><div class=\"val\" id=\"spump\">-</div></div>");
  html += F("<div class=\"card\"><div class=\"label\">Fallback-AP</div><div class=\"val\" id=\"apstate\">-</div></div>");
  html += F("</div>");
  html += F("<div class=\"card\"><div class=\"label\">Rohdaten UART (letzte Zeile)</div><div id=\"raw\" style=\"font-family:monospace;word-break:break-all\">-</div></div>");
  html += F("<p style=\"margin-top:1rem\"><a href=\"/\">&#8592; Zur&uuml;ck</a></p>");
  html += F("<script>");
  html += F("function toggleDemo(){fetch('/api/demo',{method:'POST'}).then(r=>r.json()).then(d=>{");
  html += F("document.getElementById('demobtn').textContent=d.demo?'Demo stoppen':'Demo-Shot starten';");
  html += F("document.getElementById('demostatus').textContent=d.demo?'Simuliert Shot: wartet 4s, dann 30s Pumpe…':'';});}");
  html += F("function toggleAp(){var on=document.getElementById('apbtn').dataset.on!=='1';");
  html += F("fetch('/api/set-ap?enabled='+(on?'1':'0'),{method:'POST'}).then(r=>r.json()).then(d=>{");
  html += F("document.getElementById('apbtn').textContent=d.ap?'AP aus':'AP an';");
  html += F("document.getElementById('apbtn').dataset.on=d.ap?'1':'0';});}");
  html += F("function upd(){fetch('/api/status').then(r=>r.json()).then(d=>{");
  html += F("if(d.demo)document.getElementById('demostatus').textContent='Demo aktiv';");
  html += F("var apb=document.getElementById('apbtn');apb.textContent=d.ap?'AP aus':'AP an';apb.dataset.on=d.ap?'1':'0';");
  html += F("document.getElementById('reed').textContent=d.reed?'HIGH (offen)':'LOW (aktiv)';");
  html += F("document.getElementById('reed').className='val '+(d.reed?'off':'on');");
  html += F("document.getElementById('pump').textContent=d.pump?'AN':'aus';");
  html += F("document.getElementById('pump').className='val '+(d.pump?'on':'off');");
  html += F("document.getElementById('timer').textContent=d.timer+'s';");
  html += F("document.getElementById('hx').textContent=d.hx>-999?d.hx+'°C':'--';");
  html += F("document.getElementById('steam').textContent=d.steam>-999?d.steam+'°C':'--';");
  html += F("var sm={'C':'Kaffee','S':'Dampf','X':'--','':'--'};");
  html += F("document.getElementById('state').textContent=sm[d.state]||d.state;");
  html += F("document.getElementById('heat').textContent=d.heating?(d.boost?'Boost-Heizung aktiv':'Heizung aktiv'):'Heizung aus';");
  html += F("document.getElementById('psrc').textContent=d.pumpSource==='serial'?'Seriell':'Reed';");
  html += F("document.getElementById('psrc').className='val '+(d.pumpSource==='serial'?'warn':'off');");
  html += F("document.getElementById('spump').textContent=d.serialPump?'AN':'aus';");
  html += F("document.getElementById('spump').className='val '+(d.serialPump?'on':'off');");
  html += F("document.getElementById('apstate').textContent=d.ap?'AN':'aus';");
  html += F("document.getElementById('apstate').className='val '+(d.ap?'on':'off');");
  html += F("document.getElementById('raw').textContent=d.raw||'(keine Daten)';");
  html += F("});}");
  html += F("upd();setInterval(upd,500);");
  html += F("</script>");
  html += F("<p style=\"font-size:.72rem;color:#aaa;text-align:center;margin-top:1rem\">v");
  html += F(FIRMWARE_VERSION);
  html += F(" &middot; ");
  html += FIRMWARE_BUILT;
  html += F("</p>");
  html += F("</body></html>");
  server.send(200, "text/html; charset=utf-8", html);
}

static void handleApiWifiScan() {
  int n = WiFi.scanNetworks();
  String json = "[";
  for (int i = 0; i < n; i++) {
    if (i > 0) json += ",";
    int rssi = WiFi.RSSI(i);
    int bars = rssi >= -55 ? 4 : rssi >= -65 ? 3 : rssi >= -75 ? 2 : 1;
    json += "{\"ssid\":\"" + WiFi.SSID(i) + "\"";
    json += ",\"rssi\":" + String(rssi);
    json += ",\"bars\":" + String(bars);
    json += ",\"open\":" + String(WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "true" : "false");
    json += "}";
  }
  json += "]";
  WiFi.scanDelete();
  server.send(200, "application/json", json);
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
  mqttClient.publish("/marax/pump", shotRunning ? "on" : "off");
}

static void broadcastMachineHeating() {
  mqttClient.publish("/marax/machineheating", machineHeating ? "on" : "off");
}

static void broadcastMachineHeatingBoost() {
  mqttClient.publish("/marax/machineheatingboost", machineHeatingBoost ? "on" : "off");
}

static void syncTimerCountForMqtt() {
  if (shotRunning) {
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
      ((millis() - timerStartMillis) / 1000) > 15 && !shotRunning &&
      timerCount > 0) {
    lastTimerStartMillis = timerStartMillis;
    broadcastShot();
  }

  if (lastTimerStarted != shotRunning) {
    lastTimerStarted = shotRunning;
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
  // Pump state: last CSV field (index 25 in frame "C1.06,116,124,093,0840,1,0")
  if (receivedChars[25] != '\0') {
    g_serialPumpActive = (receivedChars[25] == '1');
  }
  if (millis() - lastTempSampleMs >= 5000 &&
      hxTemperature > MARAX_TEMP_INVALID && steamTemperature > MARAX_TEMP_INVALID) {
    lastTempSampleMs = millis();
    tempHistory[tempHistoryHead] = { (uint32_t)millis(), (int16_t)hxTemperature, (int16_t)steamTemperature };
    tempHistoryHead = (tempHistoryHead + 1) % TEMP_HISTORY_SIZE;
    if (tempHistoryCount < TEMP_HISTORY_SIZE) tempHistoryCount++;
  }
}

void setup() {
  LittleFS.begin(true);
  loadWifiCredentials();

  // Display vor WLAN: bei gespeichertem (falschem) WLAN blockiert tryConnectSta() bis zu 45 s —
  // sonst bleibt der Schirm schwarz, obwohl nur das OLED zum Test angeschlossen ist.
  Wire.begin(I2C_SDA, I2C_SCL);
  Serial.begin(9600);
  display.begin(SSD1306_SWITCHCAPVCC, SSD1306_I2C_ADDR);
  display.clearDisplay();
  display.setTextColor(WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println(F("MaraX-Timer v" FIRMWARE_VERSION));
  display.println(F("WLAN..."));
  display.display();

  setupWifiAndServices();
  if (g_configPortalMode || WiFi.status() == WL_CONNECTED) {
    setupWebServer();
  }

  MaraXSerial.begin(9600, SERIAL_8N1, MARAX_RX, MARAX_TX);

  pinMode(REED_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(REED_PIN), onReedChanged, CHANGE);

  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);

  memset(receivedChars, 0, numChars);

  display.clearDisplay();
  display.setTextColor(WHITE);
  display.display();
  MaraXSerial.write(0x11);
}

void loop() {
  static unsigned long lastDisplayMs = 0;
  if (millis() - lastDisplayMs >= 100) {
    lastDisplayMs = millis();
    updateDisplay();
  }
  updateDemo();
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

// Returns true when pump is actively running, regardless of source.
static bool readPumpState() {
  if (g_useSerialPump) return g_serialPumpActive;
  bool pinHigh = (bool)digitalRead(REED_PIN);
  return reedOpenSensor ? !pinHigh : pinHigh;
}

void detectChanges() {
  if (g_demoMode) return;
  if (reedInterruptPending) {
    reedInterruptPending = false;
  }

  bool pumpRunning = readPumpState();
  pumpInValue = pumpRunning ? 0 : 1;
  digitalWrite(LED_BUILTIN, pumpRunning ? LOW : HIGH);

  if (!shotRunning && pumpRunning) {
    timerStartMillis = millis();
    shotRunning = true;
    displayOn = true;
    Serial.println("Start pump");
  }
  if (shotRunning && !pumpRunning) {
    if (timerStopMillis == 0) {
      timerStopMillis = millis();
    }
    if (millis() - timerStopMillis > 500) {
      int durationSec = (int)((millis() - timerStartMillis) / 1000);
      if (durationSec >= 15) {
        appendShotHistory(durationSec);
      }
      shotRunning = false;
      timerStopMillis = 0;
      timerDisplayOffMillis = millis();
      display.invertDisplay(false);
      Serial.println("Stop pump");
    }
  } else {
    timerStopMillis = 0;  // pump still running — reset debounce timer
  }
  if (!shotRunning && displayOn && timerDisplayOffMillis >= 0 &&
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
  if (shotRunning) {
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

static void drawSparkline() {
  display.clearDisplay();
  display.setTextSize(1);

  // Kopfzeile: HX-Temp + Bereit-Indikator
  display.setCursor(0, 0);
  if (hxTemperature > MARAX_TEMP_INVALID) {
    display.print(F("HX:"));
    display.print(hxTemperature);
    display.print((char)247);
    display.print(F("C"));
  }
  bool atTemp = hxTemperature != MARAX_TEMP_INVALID && hxTemperature >= targetHxTemp;
  if (atTemp) {
    display.setCursor(66, 0);
    display.print(F("* BEREIT"));
  }
  display.setCursor(0, 9);
  if (steamTemperature > MARAX_TEMP_INVALID) {
    display.print(F("D:"));
    display.print(steamTemperature);
    display.print((char)247);
    display.print(F("C"));
  }
  if (atTemp && g_readySinceMs > 0) {
    unsigned long seit = (millis() - g_readySinceMs) / 1000;
    display.setCursor(66, 9);
    display.print(F("seit "));
    display.print((int)(seit / 60));
    display.print(F(":"));
    if (seit % 60 < 10) display.print(F("0"));
    display.print((int)(seit % 60));
  }

  if (tempHistoryCount < 2) {
    display.setCursor(20, 35);
    display.print(F("keine Daten"));
    display.display();
    return;
  }

  // Graphbereich y=20..61, x=0..127
  const int GY0 = 20, GY1 = 61, GW = 128;
  const int GH = GY1 - GY0;
  int minT = 200, maxT = 0;
  int start = tempHistoryCount < TEMP_HISTORY_SIZE ? 0 : tempHistoryHead;
  for (int i = 0; i < tempHistoryCount; i++) {
    int idx = (start + i) % TEMP_HISTORY_SIZE;
    minT = min(minT, (int)tempHistory[idx].hx);
    maxT = max(maxT, (int)tempHistory[idx].hx);
  }
  minT = min(minT, targetHxTemp - 3);
  maxT = max(maxT, targetHxTemp + 3);
  int tRange = max(maxT - minT, 1);

  // Ziel-Linie gestrichelt
  int targY = GY1 - (targetHxTemp - minT) * GH / tRange;
  for (int x = 0; x < GW; x += 4)
    display.drawPixel(x, targY, SSD1306_WHITE);

  // Sparkline HX
  int drawN = min(tempHistoryCount, GW);
  int si = (start + max(0, tempHistoryCount - drawN)) % TEMP_HISTORY_SIZE;
  int px = -1, py = -1;
  for (int i = 0; i < drawN; i++) {
    int idx = (si + i) % TEMP_HISTORY_SIZE;
    int x = i * (GW - 1) / max(drawN - 1, 1);
    int y = GY1 - (tempHistory[idx].hx - minT) * GH / tRange;
    y = max(GY0, min(GY1, y));
    if (px >= 0) display.drawLine(px, py, x, y, SSD1306_WHITE);
    px = x; py = y;
  }

  // Achsenbeschriftung
  display.setCursor(0, 56);
  display.print(minT);
  display.setCursor(104, 56);
  display.print(maxT);
  display.display();
}

// Coffee cup icon (26px wide, 23px tall from top-left corner x,y)
static void drawCup(int16_t x, int16_t y) {
  // Steam wisps
  display.drawLine(x+6,  y+5, x+7,  y+2, SSD1306_WHITE);
  display.drawLine(x+7,  y+2, x+6,  y+0, SSD1306_WHITE);
  display.drawLine(x+13, y+5, x+14, y+2, SSD1306_WHITE);
  display.drawLine(x+14, y+2, x+13, y+0, SSD1306_WHITE);
  // Rim
  display.drawLine(x+1,  y+7, x+20, y+7, SSD1306_WHITE);
  // Left & right walls
  display.drawLine(x+2,  y+8, x+2,  y+20, SSD1306_WHITE);
  display.drawLine(x+19, y+8, x+19, y+20, SSD1306_WHITE);
  // Bottom
  display.drawLine(x+2, y+20, x+19, y+20, SSD1306_WHITE);
  // Handle
  display.drawLine(x+19, y+10, x+23, y+10, SSD1306_WHITE);
  display.drawLine(x+23, y+10, x+23, y+17, SSD1306_WHITE);
  display.drawLine(x+23, y+17, x+19, y+17, SSD1306_WHITE);
  // Saucer
  display.drawLine(x+0, y+22, x+25, y+22, SSD1306_WHITE);
}

// Steam cloud icon (26px wide, 18px tall; cx/cy = horizontal center, bottom of cloud)
static void drawSteamCloud(int16_t cx, int16_t cy) {
  // Rising steam trails
  display.drawLine(cx-3, cy-12, cx-2, cy-16, SSD1306_WHITE);
  display.drawLine(cx-2, cy-16, cx-3, cy-19, SSD1306_WHITE);
  display.drawLine(cx+4, cy-12, cx+5, cy-16, SSD1306_WHITE);
  display.drawLine(cx+5, cy-16, cx+4, cy-19, SSD1306_WHITE);
  // Cloud bumps
  display.fillCircle(cx-8, cy-5,  5, SSD1306_WHITE);
  display.fillCircle(cx+1, cy-8,  6, SSD1306_WHITE);
  display.fillCircle(cx+9, cy-4,  5, SSD1306_WHITE);
  // Flat base
  display.fillRect(cx-13, cy-5, 27, 6, SSD1306_WHITE);
}

void updateDisplay() {
  // Ready-tracking
  bool atTemp = hxTemperature != MARAX_TEMP_INVALID && hxTemperature >= targetHxTemp;
  if (atTemp && !g_wasAtTemp) {
    g_readySinceMs = millis();
    g_showSparkline = false;
    g_lastDisplaySwitch = millis();
    displayOn = true;
  }
  if (!atTemp) g_readySinceMs = 0;
  g_wasAtTemp = atTemp;

  // Cycle idle <-> sparkline (not during shot, not when at target temp)
  if (!shotRunning && !atTemp && displayOn && millis() - g_lastDisplaySwitch >= 10000) {
    g_lastDisplaySwitch = millis();
    g_showSparkline = !g_showSparkline;
  }
  if (shotRunning) g_showSparkline = false;
  if (g_showSparkline && !shotRunning && displayOn) { drawSparkline(); return; }

  display.clearDisplay();
  if (!displayOn) { display.display(); return; }

  if (shotRunning) {
    unsigned long elapsed = (millis() - timerStartMillis) / 1000;
    bool preInf = elapsed < 5;
    int16_t topY = 0;

    if (preInf) {
      display.setTextSize(1);
      display.setCursor(14, 0);
      display.print(F("-- Pre-Infusion --"));
      topY = 10;
    }

    // Vertical divider
    display.drawLine(63, topY, 63, 63, SSD1306_WHITE);

    // Left: cup icon
    drawCup(4, topY + 2);

    // Left bottom: HX temp small
    display.setTextSize(1);
    display.setCursor(2, 55);
    display.print(F("HX:"));
    if (hxTemperature > MARAX_TEMP_INVALID) {
      display.print(hxTemperature);
      display.print((char)247);
    } else {
      display.print(F("--"));
    }

    // Right: large timer centered in right panel
    String t = getTimer();
    uint8_t ts = preInf ? 3 : 4;
    int16_t tw = t.length() * (preInf ? 18 : 24);
    int16_t tx = 65 + (61 - tw) / 2;
    int16_t th = preInf ? 24 : 32;
    int16_t ty = topY + (63 - topY - th) / 2;
    display.setTextSize(ts);
    display.setCursor(tx, ty);
    display.print(t);

  } else {
    // Idle: two-column layout
    display.drawLine(63, 0, 63, 63, SSD1306_WHITE);

    // Left: cup icon + HX temp
    drawCup(4, 2);
    display.setTextSize(2);
    display.setCursor(2, 46);
    if (hxTemperature > MARAX_TEMP_INVALID) {
      display.print(hxTemperature);
      display.print((char)247);
    } else {
      display.print(F("--"));
    }

    // Right: "Zeit:XX" header
    display.setTextSize(1);
    display.setCursor(66, 2);
    display.print(F("Zeit:"));
    display.print(getTimer());

    // Right: steam cloud icon
    drawSteamCloud(95, 42);

    // Right: steam temp at bottom
    display.setTextSize(1);
    display.setCursor(66, 56);
    if (steamTemperature > MARAX_TEMP_INVALID) {
      display.print(steamTemperature);
      display.print((char)247);
      display.print(F("C"));
    } else {
      display.print(F("-- C"));
    }
  }

  display.display();
}
