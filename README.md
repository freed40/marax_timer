# marax_timer

**[Deutsch](#deutsch)** · **[English](#english)**

---

<a id="deutsch"></a>

## Deutsch

Shot-Timer und Statusanzeige für die **Lelit Mara X** (z. B. PL62X) und ähnliche Maschinen mit **Vibrationspumpe**. Das OLED-Display zeigt Laufzeit, Brühkreis-/Dampftemperaturen und Heizstatus. Steuerung und OTA-Updates über den Browser. Basiert auf **[espresso_timer](https://github.com/alexrus/espresso_timer)**.

### In diesem Repository

| Sketch | Plattform | Ordner | Kurzbeschreibung |
|--------|-----------|--------|------------------|
| **ESP32** | ESP32 DevKit V1 | [`timer_esp32/`](timer_esp32/) | WLAN per Browser, Weboberfläche, Shot-Verlauf, MQTT, OTA |
| **Klassisch** | ESP8266 (NodeMCU) | [`timer/`](timer/) | WiFi aus, SoftwareSerial zur Mara X |

### Funktionsweise

Der Timer kann auf zwei Arten ausgelöst werden — umschaltbar per Weboberfläche ohne erneutes Flashen:

- **Reed-Sensor-Modus** (Standard): Ein Reed-Sensor am Pumpenkörper erkennt das Magnetfeld der laufenden Pumpe.
- **Seriell-Modus**: Der Pumpenstatus wird direkt aus den UART-Daten der Mara X gelesen (Byte 25 im Frame `C1.06,116,124,093,0840,1,0`). Kein Reed-Sensor notwendig.

Shots unter 15 Sekunden (z. B. Boiler-Nachfüllen) werden nicht gespeichert. Nach 1 Stunde Inaktivität wechselt das Display in den Ruhezustand.

### Hardware

#### Gemeinsam

- **0,96″ OLED**, SSD1306, I²C (4 Pins: VCC, GND, SDA, SCL)
- **Reed-Sensor-Modul** (4 Pins: +, G, D0, A0) — typisch NO-Typ, optional im Seriell-Modus
- Reed am **Pumpenkörper** anbringen — das Magnetfeld der laufenden Pumpe schaltet den Kontakt:

![](resources/pump.jpg)

#### ESP32 DevKit V1 — Pinbelegung

| Komponente | Modul-Pin | ESP32-Pin | Hinweis |
|------------|-----------|-----------|---------|
| Reed-Modul | **+** | **3,3 V** | Versorgung |
| Reed-Modul | **G** | **GND** | |
| Reed-Modul | **D0** | **GPIO 18** | Digitalsignal, Interrupt |
| Reed-Modul | **A0** | — | nicht belegt |
| OLED | **VCC** | **3,3 V** | |
| OLED | **GND** | **GND** | |
| OLED | **SDA** | **GPIO 21** | I²C Daten |
| OLED | **SCL** | **GPIO 22** | I²C Takt |
| Mara X Pin 3 (RX) | — | **GPIO 12** | UART TX vom ESP |
| Mara X Pin 4 (TX) | — | **GPIO 14** | UART RX zum ESP |

> **OLED-Adresse:** Die meisten 128×64-Module verwenden `0x3C`. Bei schwarzem Display `SSD1306_I2C_ADDR` auf `0x3D` ändern.

> **Mara X UART:** Der 6-polige Stecker sitzt an der Unterseite der Maschine. Falls keine Daten ankommen, RX/TX tauschen (GPIO 12 ↔ GPIO 14).

> **GPIO 12:** Strapping-Pin — bei Boot-Problemen einen anderen freien Pin verwenden und `MARAX_TX` im Sketch anpassen.

#### ESP8266 NodeMCU — Pinbelegung

| Komponente | NodeMCU-Pin | GPIO |
|------------|-------------|------|
| Reed Signal | D7 | GPIO 13 |
| OLED SDA | D2 | GPIO 4 |
| OLED SCL | D1 | GPIO 5 |
| Mara X RX | D6 | GPIO 12 |
| Mara X TX | D5 | GPIO 14 |

### OLED-Display

**Leerlauf** — zweigeteilte Ansicht (Trennlinie bei x=63):

| Links | Rechts |
|-------|--------|
| Kaffeetassen-Icon mit Dampfkringeln | `Zeit:XX` |
| HX-Temperatur groß | Dampfwolken-Icon |
| | Dampftemperatur |

**Shot läuft:**
- Erste 5 Sekunden: `-- Pre-Infusion --` als Kopfzeile, Tasse links, Timer rechts
- Ab 5 Sekunden: Tasse links, großer Timer rechts

Das OLED wechselt alle 10 Sekunden zwischen Normalanzeige und Temperatur-Sparkline (nur im Leerlauf).

### Software-Installation (arduino-cli)

#### 1. arduino-cli installieren

```bash
brew install arduino-cli
```

#### 2. ESP32-Board-Paket einrichten

```bash
arduino-cli config init
arduino-cli config add board_manager.additional_urls \
  https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
arduino-cli core update-index
arduino-cli core install esp32:esp32@2.0.17
```

> Version 2.0.17 empfohlen — neuere Versionen laden einen großen RISC-V-Toolchain nach (>500 MB), der für den Standard-ESP32 nicht benötigt wird.

#### 3. Bibliotheken installieren

```bash
arduino-cli lib install "Adafruit SSD1306" "Adafruit GFX Library" "PubSubClient"
```

#### 4. Kompilieren

```bash
arduino-cli compile --fqbn esp32:esp32:esp32 timer_esp32/
```

#### 5. Flashen (USB)

```bash
arduino-cli upload -p /dev/cu.usbserial-XXX \
  --fqbn esp32:esp32:esp32:UploadSpeed=460800 timer_esp32/
```

Port auf macOS: `/dev/cu.usbserial-*` — mit `ls /dev/cu.usb*` prüfen.  
Bei Fehler `Unable to verify flash chip connection`: BOOT-Taste halten während Upload startet, oder Baudrate auf `115200` reduzieren.

### Firmware-Update ohne Kabel (OTA)

Sobald der ESP32 einmal per USB geflasht und im WLAN ist, können alle weiteren Updates kabellos per Browser eingespielt werden.

**WLAN bleibt erhalten:** Die NVS-Partition wird von OTA nicht überschrieben — SSID, Passwort und alle gespeicherten Einstellungen (Pump-Modus, Zielwerte, AP-Status) bleiben nach jedem Update erhalten.

#### Binary erstellen

```bash
arduino-cli compile \
  --fqbn esp32:esp32:esp32 \
  --export-binaries \
  timer_esp32/
# Datei: timer_esp32/build/esp32.esp32.esp32/timer_esp32.ino.bin
```

#### Im Browser hochladen

1. Browser öffnen: **`http://marax.local/update`**
2. **Token** eingeben (Wert von `OTA_UPDATE_TOKEN` im Sketch, Standard: `maraxota`)
3. **Datei wählen** → die `.bin` aus dem Build-Schritt
4. **Hochladen** → ESP32 startet automatisch neu

#### GitHub Actions

Bei jedem Push auf `main` wird automatisch kompiliert. Bei einem Tag `v*` wird ein GitHub Release mit der Binary erstellt:

```bash
git tag v1.2.0 && git push origin v1.2.0
```

Artifacts: Actions → Lauf → `firmware-esp32`

#### Fehlersuche OTA

| Problem | Lösung |
|---------|--------|
| `/update` zeigt „OTA deaktiviert" | `OTA_UPDATE_TOKEN` setzen, per USB flashen |
| Upload schlägt fehl | Nur `timer_esp32.ino.bin` verwenden, nicht `bootloader.bin` |
| Seite nicht erreichbar nach Update | `http://marax.local/` versuchen oder IP im Router nachschlagen |

### Konfiguration im Sketch

| Variable | Standard | Beschreibung |
|----------|----------|--------------|
| `WIFI_SSID` / `WIFI_PASSWORD` | `""` | Optional: WLAN fest im Sketch (sonst per Browser) |
| `CONFIG_AP_PASSWORD` | `"maraxsetup"` | Passwort für den Konfig-AP (min. 8 Zeichen) |
| `OTA_UPDATE_TOKEN` | `""` | Token für Web-OTA — leer = OTA gesperrt |
| `MQTT_BROKER` | `""` | IP/Hostname des MQTT-Brokers |
| `reedOpenSensor` | `true` | `true` = NO-Sensor, `false` = NC-Sensor |
| `SSD1306_I2C_ADDR` | `0x3C` | I²C-Adresse des Displays |

### Weboberfläche (ESP32)

Nach dem Start im Heimnetz erreichbar unter `http://marax.local/` oder der IP-Adresse.

#### Seiten

| URL | Funktion |
|-----|----------|
| `/` | Hauptseite: Live-Pumpenstatus, Shot-Verlauf, Ziel-Einstellungen |
| `/test` | **Diagnose:** Reed, Pumpe, Temperaturen, Pump-Quelle, AP-Status, UART-Rohdaten |
| `/temps` | Temperatur-Graph (Canvas, 10 Min. Verlauf) |
| `/wifi` | WLAN ändern inkl. **Netzwerk-Scan mit Signalstärke** |
| `/update` | Firmware per Browser (OTA) — nur mit gesetztem Token |

#### API-Endpunkte

| Endpunkt | Methode | Beschreibung |
|----------|---------|--------------|
| `/api/status` | GET | JSON: Pumpe, Timer, Reed, Temp., Pump-Quelle, AP-Status, Demo |
| `/api/history` | GET | JSON: Shot-Verlauf |
| `/api/history.csv` | GET | CSV-Download des Shot-Verlaufs |
| `/api/temp-history` | GET | JSON: Temperatur-Ringpuffer (120 Messpunkte × 5 s) |
| `/api/wifi-scan` | GET | JSON: verfügbare WLAN-Netzwerke mit RSSI |
| `/api/set-target` | POST | `shot=27&hx=93` — Zielwerte speichern |
| `/api/set-pump-source` | POST | `source=reed` oder `source=serial` — Pump-Quelle umschalten |
| `/api/set-ap` | POST | `enabled=1` oder `enabled=0` — Fallback-AP ein/ausschalten |
| `/api/demo` | POST | Demo-Modus umschalten |
| `/api/clear` | POST | Shot-Verlauf löschen |

#### Erste WLAN-Einrichtung

1. ESP32 startet als Access Point **`MaraX-Timer`** (Passwort: `maraxsetup`)
2. Mit Handy/PC verbinden → Browser: `http://192.168.4.1/`
3. Heim-WLAN aus der Liste wählen, Passwort eingeben, speichern
4. ESP32 startet neu — neue IP im Router-DHCP oder `http://marax.local/`

### Pump-Quelle umschalten

```bash
# Auf Seriell-Modus umschalten (kein Reed-Sensor notwendig)
curl -X POST "http://marax.local/api/set-pump-source?source=serial"

# Zurück auf Reed
curl -X POST "http://marax.local/api/set-pump-source?source=reed"
```

Die Einstellung wird in NVS gespeichert und überlebt OTA-Updates und Neustarts. Umschalten auch über `/test` im Browser möglich.

### Fallback-AP

Der ESP32 betreibt parallel zum Heimnetz einen eigenen WLAN-Hotspot (`MaraX-Timer`), über den Diagnose (`/test`) und OTA (`/update`) immer erreichbar sind — auch wenn das Heimnetz nicht verfügbar ist.

| Situation | Verhalten |
|-----------|-----------|
| Heimnetz verbunden, AP aktiv | `marax.local` **und** `192.168.4.1` erreichbar |
| Heimnetz verbunden, AP deaktiviert | Nur `marax.local` |
| Heimnetz nicht verfügbar | Konfig-AP startet immer automatisch (nicht abschaltbar) |

AP ein-/ausschalten: `/test` → Button **„AP aus/an"** oder `POST /api/set-ap?enabled=0`.

### MQTT / Home Assistant

`MQTT_BROKER` im Sketch setzen. Topics (kompatibel mit [alexander-heimbuch/marax_timer](https://github.com/alexander-heimbuch/marax_timer)):

| Topic | Werte |
|-------|-------|
| `/marax/pump` | `on` / `off` |
| `/marax/hx` | Temperatur in °C |
| `/marax/steam` | Temperatur in °C |
| `/marax/shot` | Dauer in Sekunden |
| `/marax/machineheating` | `on` / `off` |
| `/marax/machineheatingboost` | `on` / `off` |

### Fehlersuche

| Problem | Lösung |
|---------|--------|
| Timer startet von allein | `reedOpenSensor = false` setzen (NC-Sensor) |
| Timer reagiert nicht | `reedOpenSensor = true` setzen (NO-Sensor) |
| Display bleibt schwarz | I²C-Adresse prüfen: `0x3C` oder `0x3D` |
| Keine Mara X-Daten | GPIO 12 und 14 tauschen |
| Flash schlägt fehl | BOOT-Taste halten beim Upload, oder Baudrate auf `115200` |
| Reed-Sensor zu empfindlich | Blaues Poti auf dem Sensor-Modul justieren |
| Pumpe im Seriell-Modus nicht erkannt | UART-Rohdaten auf `/test` prüfen — Byte 25 muss `0` oder `1` sein |

### Lizenz & Quellen

Siehe [LICENSE](LICENSE).

- [alexrus / espresso_timer](https://github.com/alexrus/espresso_timer)
- [alexander-heimbuch / marax_timer](https://github.com/alexander-heimbuch/marax_timer)
- [SaibotFlow / marax-monitor](https://github.com/SaibotFlow/marax-monitor) — Protokoll-Referenz serieller Pump-Status
- [Home-Barista Forum](https://www.home-barista.com/espresso-machines/lelit-marax-t61215-350.html#p723763)
- [YouTube](https://www.youtube.com/watch?v=e9FXYfr5ro4&t=526s)

---

<a id="english"></a>

## English

Shot timer and status display for the **Lelit Mara X** (e.g. PL62X) and similar machines with a **vibration pump**. The OLED shows shot time, brew/steam temperatures, and heating status. Fully configurable and updatable via browser. Based on **[espresso_timer](https://github.com/alexrus/espresso_timer)**.

### What's in this repository

| Sketch | Platform | Folder | Summary |
|--------|----------|--------|---------|
| **ESP32** | ESP32 DevKit V1 | [`timer_esp32/`](timer_esp32/) | WiFi via browser, web UI, shot history, MQTT, OTA |
| **Classic** | ESP8266 (NodeMCU) | [`timer/`](timer/) | No WiFi, SoftwareSerial to Mara X |

### How it works

The timer can be triggered in two ways — switchable via the web UI without re-flashing:

- **Reed sensor mode** (default): A reed switch mounted on the pump body detects the pump's magnetic field.
- **Serial mode**: Pump state is read directly from the Mara X UART data stream (byte 25 in the frame `C1.06,116,124,093,0840,1,0`). No reed sensor required.

Shots under 15 seconds (e.g. boiler refills) are not saved. After 1 hour of inactivity the display enters sleep mode.

### Hardware

#### Common parts

- **0.96″ OLED**, SSD1306, I²C (4 pins: VCC, GND, SDA, SCL)
- **Reed sensor module** (4 pins: +, G, D0, A0) — typically NO type, optional in serial mode
- Mount the reed on the **pump body** — the running pump's magnetic field closes the contact:

![](resources/pump.jpg)

#### ESP32 DevKit V1 — pinout

| Component | Module pin | ESP32 pin | Notes |
|-----------|------------|-----------|-------|
| Reed module | **+** | **3.3 V** | Power |
| Reed module | **G** | **GND** | |
| Reed module | **D0** | **GPIO 18** | Digital signal, interrupt |
| Reed module | **A0** | — | not used |
| OLED | **VCC** | **3.3 V** | |
| OLED | **GND** | **GND** | |
| OLED | **SDA** | **GPIO 21** | I²C data |
| OLED | **SCL** | **GPIO 22** | I²C clock |
| Mara X pin 3 (RX) | — | **GPIO 12** | UART TX from ESP |
| Mara X pin 4 (TX) | — | **GPIO 14** | UART RX to ESP |

> **OLED address:** Most 128×64 modules use `0x3C`. If the display stays blank, set `SSD1306_I2C_ADDR` to `0x3D`.

> **Mara X UART:** The 6-pin connector is on the underside of the machine. If you get no data, swap RX/TX (GPIO 12 ↔ GPIO 14).

> **GPIO 12:** ESP32 strapping pin — if boot fails, use another free pin and adjust `MARAX_TX` in the sketch.

#### ESP8266 NodeMCU — pinout

| Component | NodeMCU pin | GPIO |
|-----------|-------------|------|
| Reed signal | D7 | GPIO 13 |
| OLED SDA | D2 | GPIO 4 |
| OLED SCL | D1 | GPIO 5 |
| Mara X RX | D6 | GPIO 12 |
| Mara X TX | D5 | GPIO 14 |

### OLED Display

**Idle** — two-column layout (divider at x=63):

| Left | Right |
|------|-------|
| Coffee cup icon with steam wisps | `Time:XX` |
| HX temperature (large) | Steam cloud icon |
| | Steam temperature |

**Shot running:**
- First 5 seconds: `-- Pre-Infusion --` header, cup left, timer right
- After 5 seconds: cup left, large timer right

The OLED alternates between the normal view and a temperature sparkline every 10 seconds (idle only).

### Software setup (arduino-cli)

#### 1. Install arduino-cli

```bash
brew install arduino-cli
```

#### 2. Set up ESP32 board package

```bash
arduino-cli config init
arduino-cli config add board_manager.additional_urls \
  https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
arduino-cli core update-index
arduino-cli core install esp32:esp32@2.0.17
```

> Version 2.0.17 recommended — newer versions download a large RISC-V toolchain (>500 MB) not needed for the standard ESP32.

#### 3. Install libraries

```bash
arduino-cli lib install "Adafruit SSD1306" "Adafruit GFX Library" "PubSubClient"
```

#### 4. Compile

```bash
arduino-cli compile --fqbn esp32:esp32:esp32 timer_esp32/
```

#### 5. Flash (USB)

```bash
arduino-cli upload -p /dev/cu.usbserial-XXX \
  --fqbn esp32:esp32:esp32:UploadSpeed=460800 timer_esp32/
```

On macOS check port with `ls /dev/cu.usb*`.  
If you see `Unable to verify flash chip connection`: hold the BOOT button while upload starts, or reduce baud rate to `115200`.

### Firmware update without a cable (OTA)

Once the ESP32 has been flashed via USB and joined your WiFi, all further updates can be done wirelessly.

**WiFi survives OTA:** The NVS partition is never overwritten — SSID, password, and all saved settings (pump mode, target values, AP state) are preserved after every update.

#### Build the binary

```bash
arduino-cli compile \
  --fqbn esp32:esp32:esp32 \
  --export-binaries \
  timer_esp32/
# File: timer_esp32/build/esp32.esp32.esp32/timer_esp32.ino.bin
```

#### Upload in the browser

1. Open **`http://marax.local/update`**
2. Enter the **token** (`OTA_UPDATE_TOKEN` from the sketch, default: `maraxota`)
3. **Choose file** → the `.bin` from the build step
4. Click **Upload** → ESP32 reboots automatically

#### GitHub Actions

Every push to `main` triggers an automatic build. Pushing a tag `v*` creates a GitHub Release with the binary attached:

```bash
git tag v1.2.0 && git push origin v1.2.0
```

Artifacts: Actions → run → `firmware-esp32`

#### OTA troubleshooting

| Problem | Fix |
|---------|-----|
| `/update` shows "OTA disabled" | Set `OTA_UPDATE_TOKEN`, flash via USB |
| Upload fails | Use only `timer_esp32.ino.bin`, not `bootloader.bin` |
| Device unreachable after update | Try `http://marax.local/` or look up IP in router |

### Sketch configuration

| Variable | Default | Description |
|----------|---------|-------------|
| `WIFI_SSID` / `WIFI_PASSWORD` | `""` | Optional: hardcode WiFi credentials (otherwise via browser) |
| `CONFIG_AP_PASSWORD` | `"maraxsetup"` | Config AP password (min. 8 chars for WPA2) |
| `OTA_UPDATE_TOKEN` | `""` | OTA token — empty disables OTA |
| `MQTT_BROKER` | `""` | MQTT broker IP/hostname |
| `reedOpenSensor` | `true` | `true` = NO sensor, `false` = NC sensor |
| `SSD1306_I2C_ADDR` | `0x3C` | OLED I²C address |

### Web interface (ESP32)

Available at `http://marax.local/` or the ESP32's IP address.

#### Pages

| URL | Function |
|-----|----------|
| `/` | Main page: live pump status, shot history, target settings |
| `/test` | **Diagnostics:** reed, pump, temperatures, pump source, AP state, UART raw data |
| `/temps` | Temperature graph (Canvas, 10 min history) |
| `/wifi` | Change WiFi incl. **network scan with signal strength** |
| `/update` | Browser firmware upload (OTA) — token required |

#### API endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/api/status` | GET | JSON: pump, timer, reed, temps, pump source, AP state, demo |
| `/api/history` | GET | JSON: shot history |
| `/api/history.csv` | GET | CSV download of shot history |
| `/api/temp-history` | GET | JSON: temperature ring buffer (120 samples × 5 s) |
| `/api/wifi-scan` | GET | JSON: available networks with RSSI |
| `/api/set-target` | POST | `shot=27&hx=93` — save target values |
| `/api/set-pump-source` | POST | `source=reed` or `source=serial` — switch pump trigger |
| `/api/set-ap` | POST | `enabled=1` or `enabled=0` — toggle fallback AP |
| `/api/demo` | POST | Toggle demo mode |
| `/api/clear` | POST | Clear shot history |

#### First-time WiFi setup

1. ESP32 starts as access point **`MaraX-Timer`** (password: `maraxsetup`)
2. Connect phone/PC → browser: `http://192.168.4.1/`
3. Pick home WiFi from list, enter password, save
4. ESP32 reboots — new IP from router DHCP or `http://marax.local/`

### Switching pump source

```bash
# Switch to serial mode (no reed sensor needed)
curl -X POST "http://marax.local/api/set-pump-source?source=serial"

# Switch back to reed
curl -X POST "http://marax.local/api/set-pump-source?source=reed"
```

The setting is stored in NVS and survives OTA updates and reboots. Can also be toggled on the `/test` page.

### Fallback AP

The ESP32 runs its own WiFi hotspot (`MaraX-Timer`) alongside the home network, so diagnostics (`/test`) and OTA (`/update`) are always reachable — even if the home network is unavailable.

| Situation | Behaviour |
|-----------|-----------|
| Home WiFi connected, AP on | `marax.local` **and** `192.168.4.1` reachable |
| Home WiFi connected, AP off | Only `marax.local` |
| Home WiFi unavailable | Config AP always starts automatically (cannot be disabled) |

Toggle AP: `/test` → **"AP off/on"** button, or `POST /api/set-ap?enabled=0`.

### MQTT / Home Assistant

Set `MQTT_BROKER` in the sketch. Topics (compatible with [alexander-heimbuch/marax_timer](https://github.com/alexander-heimbuch/marax_timer)):

| Topic | Values |
|-------|--------|
| `/marax/pump` | `on` / `off` |
| `/marax/hx` | Temperature in °C |
| `/marax/steam` | Temperature in °C |
| `/marax/shot` | Duration in seconds |
| `/marax/machineheating` | `on` / `off` |
| `/marax/machineheatingboost` | `on` / `off` |

### Troubleshooting

| Problem | Fix |
|---------|-----|
| Timer starts on its own | Set `reedOpenSensor = false` (NC sensor) |
| Timer does not react | Set `reedOpenSensor = true` (NO sensor) |
| Display stays blank | Check I²C address: `0x3C` or `0x3D` |
| No Mara X data | Swap GPIO 12 and 14 |
| Flash fails | Hold BOOT during upload, or baud rate `115200` |
| Reed sensor too sensitive | Adjust the blue potentiometer on the sensor module |
| Pump not detected in serial mode | Check UART raw data on `/test` — byte 25 must be `0` or `1` |

### License & sources

See [LICENSE](LICENSE).

- [alexrus / espresso_timer](https://github.com/alexrus/espresso_timer)
- [alexander-heimbuch / marax_timer](https://github.com/alexander-heimbuch/marax_timer)
- [SaibotFlow / marax-monitor](https://github.com/SaibotFlow/marax-monitor) — serial protocol reference for pump state
- [Home-Barista forum](https://www.home-barista.com/espresso-machines/lelit-marax-t61215-350.html#p723763)
- [YouTube](https://www.youtube.com/watch?v=e9FXYfr5ro4&t=526s)
