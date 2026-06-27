# AGENTS.md — MaraX Timer

Kurz-Kontext für KI-Assistenten (Cursor, Copilot, …). Nutzer bevorzugt **Deutsch**.

## Projekt

Shot-Timer + Statusanzeige für **Lelit Mara X** (ESP32 primär, ESP8266 legacy).

| Pfad | Inhalt |
|------|--------|
| `timer_esp32/timer_esp32.ino` | Haupt-Firmware (WLAN, Web-UI, MQTT, OTA) |
| `timer/timer.ino` | ESP8266 ohne WLAN |
| `VERSION` | **Einzige Versionsquelle** (`X.Y.Z`) |
| `scripts/bump-version.sh` | Version hochzählen + Commit + Git-Tag |
| `scripts/suggest-version-bump.sh` | Empfehlung patch/minor/major aus Git-Diff |
| `CHANGELOG.md` | **Neuerungen** pro Version; `[Unreleased]` vor Release pflegen |
| `docs/MARAX_SERIAL_PROTOCOL.md` | UART-Protokoll, Pumpen-Byte, Firmware-Unterschiede |
| `.github/workflows/build-firmware.yml` | CI: Tag `v*` → GitHub Release + `.bin` |

## Hardware (ESP32)

- Reed: GPIO 18 | OLED I²C: 21/22 | Mara X UART: **RX=16, TX=17** (nicht 12/14)
- Seriell-Modus: Pumpenstatus aus UART-Feld [6] (nur fw 1.06+)

## Versionierung — SemVer-Strategie

**Niemals** `FIRMWARE_VERSION` im Sketch manuell ändern. Immer `./scripts/bump-version.sh`.

| Stufe | Wann | Beispiele |
|-------|------|-----------|
| **patch** | Bugfix, Doku, README, Bilder, CI, Refactor ohne Verhaltenänderung | Typo, Protokoll-Doku, `.gitignore` |
| **minor** | Neues Feature, neue API/Web-Route, Display-UI, MQTT-Erweiterung (abwärtskompatibel) | `/temps`, Seriell-Pump-Modus, Sparkline |
| **major** | Breaking Change für Nutzer/Flash | Pinout, NVS-Keys, MQTT-Topic-Schema, Partition, entfernte URLs |

### Release-Ablauf (Agent)

1. Neuerungen unter **`CHANGELOG.md` → `[Unreleased]`** eintragen (Kategorien: Hinzugefügt / Geändert / Behoben / Dokumentation).
2. Alle Änderungen committen (ohne Version).
3. Empfehlung holen: `./scripts/suggest-version-bump.sh`
4. Version setzen: `./scripts/bump-version.sh <patch\|minor\|major>` — verschiebt `[Unreleased]` in die neue Version.
5. Nur mit expliziter Nutzerfreigabe: `--push`

### Prüfungen vor Release

- `VERSION` = `FIRMWARE_VERSION` in `timer_esp32.ino`
- Tag `vX.Y.Z` existiert noch nicht (`git tag -l`)
- Keine Secrets im Commit (`WIFI_PASSWORD`, Tokens nur als Platzhalter)

## Konventionen

- Antworten an Nutzer: **Deutsch**
- Minimaler Diff, bestehenden Stil in `.ino` beibehalten
- Build-Artefakte: `build/`, `firmware-*` in `.gitignore`
- Commits nur auf Nutzeranfrage
