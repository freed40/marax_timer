# marax_timer

Shot-Timer und Statusanzeige für die **Lelit Mara X** (z. B. PL62X) und ähnliche Maschinen mit **Vibrationspumpe**. Das Display zeigt Laufzeit, Brühkreis-/Dampftemperaturen und Heizstatus (sofern die Maschine per UART Daten liefert). Die Logik basiert auf dem Projekt **[espresso_timer](https://github.com/alexrus/espresso_timer)** (Community-Umschreibung mit klarerer Doku).

---

## In diesem Repository

| Sketch | Plattform | Ordner | Kurzbeschreibung |
|--------|-----------|--------|------------------|
| **Klassisch** | ESP8266 (z. B. NodeMCU) | [`timer/`](timer/) | Original-Flow: WiFi aus, `SoftwareSerial` zur Mara X |
| **ESP32** | ESP32 DevKit V1 | [`timer_esp32/`](timer_esp32/) | Wie oben; **WLAN per Browser** (NVS + Konfig-AP), **Web** inkl. **Shot-Verlauf** (LittleFS), optional **MQTT / Home Assistant** |

In der **Arduino IDE** jeweils den **Sketch-Ordner** öffnen (pro Ordner nur eine `.ino`-Datei).

---

## Funktionsweise

- **Reed-Sensor** am Gehäuse der Vibrationspumpe: Sobald die Pumpe läuft und der Kontakt auslöst, startet der **Timer nach oben**. Nach Ende des Pumpvorgangs (Kontakt weg, kurze Entprellung) **stoppt** die Anzeige und zeigt die Zeit **weiter an** (wie im Original).
- **Kurze Pumpstöße** der Mara X (z. B. Boiler-Nachfüllen) unter **ca. 15 Sekunden** werden **nicht** dauerhaft als „Shot“ gespeichert bzw. für die Daueranzeige verwendet — damit bleibt die Anzeige nicht von Refill-Zyklen „verstellt“.
- Nach **ca. 1 Stunde** ohne Pumpaktivität kann das Display in den **Ruhezustand** wechseln; die nächste Pumpenaktivität weckt es wieder (siehe Sketch-Logik).
- **Serielle Verbindung zur Mara X**: Es werden periodisch Anfragen gesendet (`0x11`); die Maschine antwortet mit einer Textzeile, aus der u. a. Temperaturen und Betriebsart gelesen werden — wie im Ursprungsprojekt.

---

## Hardware

### Gemeinsam

- **0,96″ OLED**, SSD1306, I²C (4 Anschlüsse üblich)
- **Reed-Sensor** (NO/NC je nach Typ; ggf. Variable `reedOpenSensor` im Sketch anpassen)
- Kabel; optional **Gehäuse** z. B. von [Thingiverse](https://www.thingiverse.com/thing:2937731)

Reed typischerweise am **Pumpenkörper** anbringen (Magnetfeld der laufenden Pumpe schaltet den Kontakt):

![](resources/pump.jpg)

### ESP8266 (`timer/`)

- **Board:** NodeMCU o. Ä., getestet u. a. mit **NodeMCU V2 (CP2102)**
- **OLED:** wie in vielen Anleitungen für NodeMCU (D2/D1 für I²C je nach Beschriftung — im Originalsketch `Wire` ohne explizite Pinzuweisung für NodeMCU-Defaults)
- **Reed:** ein Anschluss an **GND**, Signal an **D7** (GPIO 13 laut `#define`)
- **Mara X 6-polig (Unterseite):** Maschine **Pin 3 (RX)** → NodeMCU **D6 (TX)**; Maschine **Pin 4 (TX)** → NodeMCU **D5 (RX)**. **Tausch** von D5/D6 versuchen, falls keine Daten ankommen.

Schemabild OLED/NodeMCU (Pinreihenfolge am Modul beachten):

![](https://circuits4you.com/wp-content/uploads/2019/01/NodeMCU_ESP8266_OLED_Display.png)

### ESP32 DevKit V1 (`timer_esp32/`)

| Signal | Anschluss |
|--------|-----------|
| Reed-Modul **VCC** | **3,3 V** |
| Reed **GND** | **GND** |
| Reed **D0** | **GPIO 18** (Interrupt `CHANGE`) |
| Reed **A0** | unbelegt |
| OLED **SDA** | **GPIO 21** |
| OLED **SCL** | **GPIO 22** |
| Mara X **RX** (Pin 3) | ESP32 **GPIO 12** (UART TX der MCU) |
| Mara X **TX** (Pin 4) | ESP32 **GPIO 14** (UART RX der MCU) |

**Hinweis:** GPIO 12 ist beim ESP32 ein Strapping-Pin; am Original-NodeMCU war diese Leitung ebenfalls belegt. Bei seltsamen Boot-Problemen alternativ andere freie Pins wählen und `MARAX_RX` / `MARAX_TX` im Sketch anpassen.

---

## Software & Bibliotheken (Arduino IDE)

### ESP8266

Board-Paket und Ersteinrichtung z. B. nach:  
[Quick Start NodeMCU ESP8266 in der Arduino IDE](https://www.instructables.com/id/Quick-Start-to-Nodemcu-ESP8266-on-Arduino-IDE/)

### ESP32

Board **„ESP32 Dev Module“** (oder passendes Paket) installieren, dann den Sketch `timer_esp32` öffnen und flashen.

### Abhängigkeiten (beide Varianten, je nach Sketch)

- [Adafruit SSD1306](https://github.com/adafruit/Adafruit_SSD1306)
- [Adafruit GFX](https://github.com/adafruit/Adafruit-GFX-Library)
- [Adafruit BusIO](https://github.com/adafruit/Adafruit_BusIO)
- [JChristensen Timer](https://github.com/JChristensen/Timer)

Zusätzlich **nur für `timer_esp32`**:

- [PubSubClient](https://github.com/knolleary/pubsubclient) (nur bei MQTT)

**Partitionsschema (Arduino IDE):** für den **lokalen Shot-Verlauf** muss **LittleFS** auf dem ESP32 vorhanden sein. Unter *Tools → Partition Scheme* z. B. **„Default 4MB with spiffs“** durch eine Variante mit **FFat/LittleFS** ersetzen oder **„Huge APP“** nur nutzen, wenn du keinen Flash-Dateispeicher brauchst — ohne LittleFS-Partition schlägt `LittleFS.begin()` fehl (History/Web können ausfallen). Gängig: **Default 4MB with spiffs (1.2MB APP/1.5MB SPIFFS)** auf Core-Versionen, die SPIFFS abbilden, oder explizit **LittleFS** wählen, so wie es dein **esp32**-Board-Paket anbietet.

### Build, Upload & neue Firmware-Versionen (ESP32)

Für stabile **OTA-Updates** dieselben **Arduino-T**ools-Einstellungen (Board, **Partitionsschema**, Flash-Größe, ggf. CPU-Takt) wie beim **ersten** USB-Flash beibehalten — sonst kann nach einem Web-Update der Start scheitern.

#### 1. Ersteinrichtung Arduino IDE

1. **ESP32-Board-Paket** installieren (vgl. offizielle Espressif-Doku zur Arduino IDE).
2. **Tools** z. B.  
   **Board:** *ESP32 Dev Module* (oder dein konkretes Board)  
   **Partition Scheme:** Variante **mit** Dateisystem für LittleFS (siehe Absatz oben)  
   **Upload Speed:** 921600; bei Timeouts **115200**  
   **Port:** USB‑Seriell des ESP (`COM…` / `ttyUSB…` / `cu.usbserial…`).
3. Bibliotheken aus der Liste oben einbinden.

#### 2. Erster Upload per USB

1. Ordner **`timer_esp32`** öffnen.
2. Optional **`OTA_UPDATE_TOKEN`** und **`CONFIG_AP_PASSWORD`** setzen (mindestens einmal per USB flashen, damit **/update** und der Konfig-AP wie gewünscht sind).
3. **Hochladen** warten bis der Upload fertig ist.
4. **Serieller Monitor** mit **9600 Baud** (wie `Serial.begin` im Sketch) nutzen.

#### 3. Neue Version erstellen & Binary für OTA

1. Änderungen speichern, **Sketch → Kompilieren** (Kompilierung fehlerfrei).
2. **Version dokumentieren:** z. B. Datum + Kurztext im Commit; mit **Git** zusätzlich **Tag** wie `v1.2.0` empfehlenswert.
3. **Sketch → Kompiliertes Binary exportieren** (*Export Compiled Binary*). Im Ordner **`timer_esp32/`** erscheint u. a. **`timer_esp32.ino.bin`** — diese Datei typischerweise für **/update** verwenden (die **Anwendungs-Firmware**, nicht eine separate reine Bootloader-`.bin`, falls die IDE mehrere Dateien erzeugt). Bei Unsicherheit die vom Core erzeugte Haupt-App-Binary laut [Espressif-/Core-Doku](https://docs.espressif.com/projects/arduino-esp32/) zum Export prüfen.
4. Binary sinnvoll umbenennen/archivieren, z. B. `marax_timer_esp32_2026-04-01.bin`.

#### 4. OTA im Browser

Gerät im Heim-WLAN oder am Konfig-AP, **`OTA_UPDATE_TOKEN`** aktiv.

1. **`http://<IP>/update`** öffnen (siehe nächster Unterabschnitt).
2. Token setzen, die erzeugte **`.bin`** hochladen, Neustart abwarten.

Schlägt OTA fehl: **USB-Flasher** mit gleichem Partitionsschema.

#### 5. ESP8266 (`timer/`)

Hier üblicherweise **nur USB-Upload** (Board *NodeMCU*, Port wählen, Hochladen). **Kein** Web-OTA in diesem Sketch.

#### 6. Automatischer Build mit GitHub Actions

Im Repository liegt **[`.github/workflows/build-firmware.yml`](.github/workflows/build-firmware.yml)**. Bei **Push** auf `main`, **Pull Requests** und manuell (**Actions → „Firmware bauen“ → Run workflow**) werden mit **arduino-cli** die Sketches **`timer_esp32`** (FQBN `esp32:esp32:esp32`) und **`timer`** (NodeMCU) kompiliert.

- **Artefakte:** Unter **Actions** den Lauf öffnen → **Artifacts** → `firmware-esp32` bzw. `firmware-esp8266` herunterladen (ZIP mit **`.bin`**-Dateien).
- **GitHub Release** läuft **nur bei einem Tag** `v*` (nicht bei jedem Push auf `main`). Normale Pushes erzeugen nur Artefakte — der Job „GitHub Release“ ist dann **Skipped**, das ist erwartet.
- **Release anlegen:** Wenn du einen **Git-Tag** im Format **`v*`** pushst (z. B. `v1.0.0`), legt der Workflow nach erfolgreichem Build automatisch ein **Release** an und hängt **beide** `.bin`-Dateien an (Release Notes werden aus Commits generiert). Beispiel:
  ```bash
  git tag v1.0.0
  git push origin v1.0.0
  ```
- Die **ESP32**-Hauptfirmware (z. B. `timer_esp32.ino.bin`) kannst du für **Web-OTA (`/update`)** oder USB verwenden — **Partitionsschema und Board** in der Workflow-Datei müssen zum **ersten** Flash auf dem Gerät passen (siehe Kommentar in der YAML).
- **Kein automatischer Flash** am ESP — nur Build, Artefakte und optional Release.

Lokal denselben Build testen: [Arduino CLI](https://arduino.github.io/arduino-cli/) installieren und die Kommandos aus der Workflow-Datei nachvollziehen.

**Falls CI fehlschlägt:** ESP8266 steckt nicht im Standard-Index — die Workflow-Datei nutzt die offizielle **package-URL** der ESP8266-Community. ESP32 nutzt die **Espressif-URL**. Die CI kompiliert mit **FQBN** `esp32:esp32:esp32` (Board *ESP32 Dev Module* im aktuellen Core; ältere Anleitungen nennen noch `…:esp32dev`). Lokal in der IDE **dieselbe** Board-Auswahl und ein **passendes** Partitionsschema wählen (siehe Abschnitt Software & Bibliotheken).

---

## Webinterface & lokaler Verlauf (nur ESP32, ohne Home Assistant)

### WLAN einrichten (ohne Sketch anpassen)

Die **Zugangsdaten für dein Heim-WLAN** werden im **Flash (NVS)** gespeichert — du kannst sie **über den Browser** setzen oder ändern (`/wifi`).

1. **Erste Einrichtung (kein NVS, optional auch leerer `WIFI_SSID` im Sketch):**  
   Der ESP startet einen **eigenen Access Point** **`MaraX-Timer`** (Passwort: **`CONFIG_AP_PASSWORD`** im Sketch, Standard z. B. `maraxsetup` — mindestens 8 Zeichen; sonst offenes AP).  
   Mit dem Handy/PC **mit diesem WLAN verbinden**, Browser öffnen: **`http://192.168.4.1/`** (Router-IP des ESP im AP-Modus).

2. **SSID und Passwort** deines Routers eintragen, speichern → Gerät startet neu und verbindet sich mit dem **Heim-WLAN**.

3. **Falsches Passwort / Verbindung schlägt fehl:** Es erscheint wieder der **Konfig-AP** `MaraX-Timer` — erneut einrichten.

4. **Später ändern:** Im Heimnetz **`http://<IP>/wifi`** oder **`http://marax.local/wifi`** (wenn mDNS funktioniert).

5. **Priorität:** Zuerst wird gelesen, was in **NVS** steht; nur wenn dort noch **keine** SSID gespeichert ist, gelten die optionalen Konstanten **`WIFI_SSID`** / **`WIFI_PASSWORD`** im Sketch (z. B. für die erste USB-Flasheinstellung).

Nach erfolgreicher Verbindung: **NTP** (UTC), **HTTP-Server** Port **80**, optional **`http://marax.local/`** (mDNS).

- **Shot-History:** Jeder Lauf **≥ 15 Sekunden** → **LittleFS** (`shots.log`) mit Zeitstempel und optional HX/Dampf. **„Verlauf löschen“** auf der Startseite.
- **API:** `GET /api/history`, `POST /api/clear`.

**Hinweis:** Kein Login auf der Webseite — nur im vertrauenswürdigen Heimnetz nutzen. **Konfig-AP-Passwort** im Sketch ändern, wenn Nachbarn in Reichweite sind.

### Firmware per Browser aktualisieren (OTA)

*(Welche `.bin` du genau brauchst und wie du neue Versionen erzeugst: Abschnitt **Build, Upload & neue Firmware-Versionen** oben.)*

1. Im Sketch **`OTA_UPDATE_TOKEN`** setzen (beliebiges Passwort) und **einmal per USB** flashen — ohne Token ist **`/update`** gesperrt.
2. Im Browser **`http://<IP>/update`** oder über den Link auf der Startseite öffnen.
3. **Sketch → Kompiliertes Binary exportieren** → passende **`timer_esp32.ino.bin`** wählen.
4. Token eingeben, hochladen; das Gerät startet neu.

**Hinweis:** Partitionsschema und App-Größe müssen zum ersten Flash passen; sonst schlägt das Update fehl. Im Heimnetz erfolgt die Übertragung **unverschlüsselt** (HTTP).

---

## MQTT & Home Assistant (nur ESP32)

WLAN muss **eingerichtet** sein (NVS oder Sketch), damit der ESP im Heimnetz ist. **`MQTT_BROKER`** im Sketch setzen — dann werden die MQTT-Topics publiziert, sobald **WLAN verbunden** ist.

*(Reiner Offline-Betrieb ohne WLAN: den **ESP8266**-Sketch `timer/` nutzen oder Firmware ohne Konfig-AP — der ESP32-Build ist auf Netzwerkdienste ausgelegt.)*

**Topics** (kompatibel zur Idee von [alexander-heimbuch/marax_timer](https://github.com/alexander-heimbuch/marax_timer)):

- `/marax/power` — `on` / `off`
- `/marax/pump` — `on` / `off`
- `/marax/hx` — Brühkreis-Temperatur (Zahl als String)
- `/marax/steam` — Dampf-Temperatur
- `/marax/shot` — letzte Shot-Dauer in Sekunden (Logik wie im Referenzprojekt)
- `/marax/machineheating` — `on` / `off`
- `/marax/machineheatingboost` — `on` / `off`

YAML-Beispiele für Sensoren in **Home Assistant** und Mosquitto stehen in der README des verlinkten Repos — **Topic-Namen und Payloads** sind so gewählt, dass du diese Konfiguration übernehmen kannst.

---

## Kalibrierung & Fehlersuche

- **Timer startet von allein oder reagiert nicht:** Reed-Typ oder Polarität passt nicht — `reedOpenSensor` im Sketch umstellen **oder** anderen Reed verwenden (NO/NC).
- **Reed-Modul mit Trimmpoti:** Vorsichtig die Empfindlichkeit drehen, während die Maschine Strom hat (nicht übertreiben).
- **Keine Mara X-Daten auf dem Display:** RX/TX zur Maschine **tauschen** (Pin 3/4 ↔ MCU-Pins wie in der Tabelle oben).

---

## Bedienoberfläche (Beispiel)

![](resources/ui.jpg)

---

## Lizenz & Quellen

Siehe [LICENSE](LICENSE) im Repository.

- Ursprungsidee / Fork-Kette: [alexrus / espresso_timer](https://github.com/alexrus/espresso_timer)
- MQTT-/HA-Ansatz: [alexander-heimbuch / marax_timer](https://github.com/alexander-heimbuch/marax_timer)

---

## Mehr Infos & Video

- Thread und Erfahrungswerte (Englisch): [Home-Barista — Lelit Mara X](https://www.home-barista.com/espresso-machines/lelit-marax-t61215-350.html#p723763)
- YouTube (eingebunden ab gewähltem Zeitpunkt):  
  [https://www.youtube.com/watch?v=e9FXYfr5ro4&t=526s](https://www.youtube.com/watch?v=e9FXYfr5ro4&t=526s)
