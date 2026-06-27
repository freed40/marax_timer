# Changelog

Alle wichtigen Änderungen an diesem Projekt. Format basiert auf [Keep a Changelog](https://keepachangelog.com/de/1.1.0/).

Vor jedem Release Einträge unter **`[Unreleased]`** pflegen. Beim Release verschiebt `./scripts/bump-version.sh` den Inhalt automatisch in die neue Versionssektion.

## [Unreleased]

### Hinzugefügt

- `CHANGELOG.md` und Release-Notes aus Changelog bei GitHub Release
- `VERSION`, `scripts/bump-version.sh`, `scripts/suggest-version-bump.sh`
- `AGENTS.md` und Cursor-Regel (`.cursor/rules/`) für SemVer und KI-Kontext

### Geändert

- CI: Firmware-Version aus `VERSION` bzw. Git-Tag; Release-Text aus Changelog

### Dokumentation

- Schnittstellen-Fotos und UART-Tabelle ([marax-monitor](https://github.com/SaibotFlow/marax-monitor))
- `docs/MARAX_SERIAL_PROTOCOL.md` (Protokoll, Pumpen-Byte, Firmware 1.06 vs. älter)
- README zweisprachig (DE/EN), Abschnitt Versionierung

## [1.1.0] - 2026-06-27

### Hinzugefügt

- **ESP32 Web-UI:** Live-Status, Shot-Verlauf, Diagnose `/test`, Temperaturgraph `/temps`, CSV-Export
- **Pumpen-Modus:** Reed-Sensor (Standard) oder Seriell (UART-Feld Pumpe, fw 1.06+), umschaltbar in der Weboberfläche
- **Zielwerte:** Ziel-Shotzeit und HX-Solltemperatur (Browser, NVS, Fortschrittsbalken)
- **OLED:** Kaffee-/Dampf-Icons, Pre-Infusion (5 s), Sparkline, „BEREIT“ bei Ziel-HX
- **Demo-Modus** auf `/test` zum Testen ohne Maschine
- **Fallback-AP** `MaraX-Timer` parallel zum Heimnetz (abschaltbar)
- **Einstellungen** exportieren/importieren (JSON: WLAN, Ziele, Pump-Quelle, AP)
- **OTA** per Browser (`/update`) mit Token; Hinweis bei neuer GitHub-Version
- **MQTT / Home Assistant** (Topics kompatibel mit [marax_timer](https://github.com/alexander-heimbuch/marax_timer))
- **GitHub Actions:** Build bei Push; Release mit `.bin` bei Tag `v*`

### Geändert

- ESP32 UART: **GPIO 16 (RX) / 17 (TX)** — GPIO 12 ist Strapping-Pin
- `.gitignore` für lokale Build-Artefakte (`build/`, `firmware-*`)

### Behoben

- Display startet vor WLAN-Verbindung (kein schwarzer Bildschirm während 45 s STA-Timeout)
