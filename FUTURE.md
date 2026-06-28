# Geplante Erweiterungen – MaraX Timer

## 1. Bookoo Wage – BLE-Waage

### Idee
Den ESP32 als BLE Central nutzen, der sich mit der Bookoo Wage verbindet und das Gewicht live anzeigt.

### Nutzen
- Gewicht auf dem OLED neben dem Timer anzeigen (z. B. `35.2 g`)
- Gewicht im Shot-Log mitschreiben (g → CSV-Spalte)
- Automatischer Pump-Stop wenn Zielgewicht erreicht (Brew-by-Weight)

### Technische Details
- ESP32 hat BLE eingebaut, kein extra Modul nötig
- Bibliothek: **NimBLE-Arduino** (deutlich kompakter als die Standard Arduino BLE-Lib)
- Das Bookoo-Wage-Protokoll ist reverse-engineered und im Projekt [`bookoo-ble`](https://github.com/nickcoutsos/bookoo-ble) dokumentiert
  - Service UUID: `FFE0` (oder `0xFFE0`)
  - Characteristic für Gewicht-Notifications: `FFE1`
  - Frames: 20 Byte, Gewicht in Byte 3–4 als uint16, Einheit g × 10
- ESP32 scannt nach BLE-Devices mit Name `"BOOKOO"` und verbindet sich automatisch

### Voraussetzungen / Aufwand
- **Partition-Tabelle anpassen**: aktuell 83% Flash mit Standard-Schema (1,25 MB App-Bereich)
  - Umstellen auf `Huge APP` Partition-Tabelle: App-Bereich 3 MB, SPIFFS nur 192 KB
  - Shot-History dadurch auf ~100 Einträge begrenzen statt unbegrenzt (unkritisch)
  - In `arduino-cli`: `--fqbn esp32:esp32:esp32:PartitionScheme=huge_app`
- FQBN in CI anpassen
- BLE-Scan und Auto-Reconnect implementieren
- OLED-Layout erweitern: eine Zeile für Gewicht reservieren

### Aufwand-Schätzung
Mittel (1–2 Tage): Partition-Umstellung + BLE-Client + OLED + Log-Erweiterung

---

## 2. Google Home – Benachrichtigung wenn Maschine bereit

### Idee
Wenn der HX-Brühkreis die Zieltemperatur erreicht (aktuell: `targetHxTemp`, Standard 93 °C), soll eine Benachrichtigung auf dem Smartphone / Google Home kommen.

### Optionen (von einfach bis komplex)

#### Option A – Google Home Routinen via Webhooks (empfohlen)
Google Home unterstützt **Webhooks** als Routinen-Auslöser (seit 2023).  
Der ESP32 macht einen HTTP-POST an einen Webhook-Endpunkt → Google Home Routine spricht eine Nachricht auf dem Lautsprecher.

Umsetzung:
1. In der Google Home App eine Routine anlegen:  
   *Auslöser*: Webhook (URL wird von Google generiert)  
   *Aktion*: „Sage auf [Gerät]: Die Mara X ist bereit!"
2. Die Webhook-URL in den MaraX-Timer-Einstellungen eintragen
3. ESP32 sendet beim Erreichen von `targetHxTemp` einen HTTP-POST an diese URL

#### Option B – MQTT → Node-RED → Google Home
Der ESP32 publisht bereits auf `/marax/hx`.  
Node-RED (auf einem Raspberry Pi oder Home Assistant) abonniert diesen Topic und sendet bei Erreichen der Zieltemperatur eine Notification via Google Home SDK oder Google Assistant Relay.

#### Option C – Home Assistant
Wenn Home Assistant läuft: MQTT-Integration → Automatisierung → Google Home Announcement via Google Cast.  
Keine Änderung am ESP32-Code nötig.

### Einfachste Umsetzung ohne Extra-Server
Option A (Webhook) direkt im ESP32:
- Beim Erreichen der Zieltemperatur: `WiFiClientSecure` → HTTP POST an den Google-Webhook
- Einmalig auslösen (nicht wiederholt), Reset wenn Temp wieder sinkt

### Aufwand-Schätzung
- Option A (Webhook direkt im ESP32): Klein (< 1 Tag), Google generiert die URL
- Option B/C (Node-RED/Home Assistant): Kein Code nötig, nur Konfiguration

---

## Priorisierung (Vorschlag)

| Feature | Aufwand | Nutzen |
|---|---|---|
| Google Home Webhook (Option A) | Klein | Hoch – tägliche Nutzung |
| BLE Waage (Bookoo Wage) | Mittel | Mittel – nur beim Espresso |
| MQTT → Home Assistant | Kein Code | Hoch – wenn HA vorhanden |
